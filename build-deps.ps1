#requires -Version 5.1
<#
.SYNOPSIS
Builds OpenSSL and libssh2 from source as static /MT libraries for the SFTP plugin.

.DESCRIPTION
Reads sources from thirdparty/openssl and thirdparty/libssh2 (git submodules).
Produces static .lib files under src/lib/ and headers under src/include/libssh2/
that the main vcxproj links against.

Build chain:
  OpenSSL: perl Configure -> nmake build_libs -> nmake install_dev
  libssh2: cmake -G Ninja (with OpenSSL backend) -> cmake --build

Each (arch, lib) step runs inside a fresh `cmd /c` invocation that activates
the matching VsDevCmd architecture, so x64 and x86 environments do not bleed
into each other within this PowerShell session.

.PARAMETER SkipOpenSSL
Skip OpenSSL build (use whatever is already in thirdparty/build/openssl-*/).

.PARAMETER SkipLibssh2
Skip libssh2 build.

.PARAMETER X64Only
Build x64 only, skip x86.

.PARAMETER Clean
Remove all thirdparty/build/ contents before building.
#>
param(
    [switch]$SkipOpenSSL,
    [switch]$SkipLibssh2,
    [switch]$X64Only,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

# ============================================================================
# Configuration
# ============================================================================
$projectRoot      = $PSScriptRoot
$thirdparty       = Join-Path $projectRoot 'thirdparty'
$opensslSrc       = Join-Path $thirdparty 'openssl'
$libssh2Src       = Join-Path $thirdparty 'libssh2'
$buildRoot        = Join-Path $thirdparty 'build'
$opensslOutX64    = Join-Path $buildRoot  'openssl-x64'
$opensslOutX86    = Join-Path $buildRoot  'openssl-x86'
$libssh2BuildX64  = Join-Path $buildRoot  'libssh2-x64'
$libssh2BuildX86  = Join-Path $buildRoot  'libssh2-x86'
$libDestDir       = Join-Path $projectRoot 'src\lib'
$includeDestDir   = Join-Path $projectRoot 'src\include\libssh2'
$vswhereDir       = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'

# Pinned version sanity (must match .gitmodules / submodule SHAs)
$expectedOpensslTag = 'openssl-3.5.6'
$expectedLibssh2Tag = 'libssh2-1.11.1'

# ============================================================================
# Helpers
# ============================================================================
function Write-Section([string]$Title) {
    Write-Host ''
    Write-Host "=== $Title ===" -ForegroundColor Cyan
}

function Write-Step([string]$Msg) {
    Write-Host "  $Msg" -ForegroundColor Gray
}

function Write-OK([string]$Msg) {
    Write-Host "  $Msg" -ForegroundColor Green
}

function Find-Tool([string]$Name, [string[]]$Candidates) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path $c)) { return $c }
    }
    return $null
}

function Find-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found. Visual Studio 2022 (or later) is required."
    }
    $vs = & $vswhere -latest -prerelease -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $vs) { throw "Visual Studio installation not found." }
    return $vs
}

function Invoke-CmdScript {
    <#
    Writes a multi-line cmd batch to a temp .bat and runs it. Confines VsDevCmd
    architecture activation to a single build step without leaking env vars
    into our own PowerShell session. Using a .bat file avoids quoting and
    line-continuation pitfalls that affect 'cmd /c "..."' invocations.
    #>
    param(
        [Parameter(Mandatory)] [string]$Body,
        [Parameter(Mandatory)] [string]$Description
    )
    Write-Step $Description
    $bat = New-TemporaryFile
    $batPath = [System.IO.Path]::ChangeExtension($bat.FullName, '.bat')
    Move-Item -Path $bat.FullName -Destination $batPath -Force
    # @echo off keeps command echoes out of build logs unless something errors;
    # 'call' on .bat files preserves the parent batch's exit propagation.
    $script = "@echo off`r`n" + $Body
    Set-Content -Path $batPath -Value $script -Encoding Default
    # Temporarily relax ErrorActionPreference: with 'Stop' in effect, PowerShell
    # treats anything a native process writes to stderr (e.g. cmake's harmless
    # deprecation warnings) as a terminating error. We only want non-zero exit
    # codes to fail the step.
    $savedPref = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # Pipe cmd output to Out-Host so it stays visible to the user but does NOT
        # leak into the calling function's success-stream output (which would
        # contaminate any value the caller returns).
        & cmd.exe /D /S /C "`"$batPath`"" 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "$Description failed (cmd exit code $LASTEXITCODE). Batch: $batPath"
        }
    } finally {
        $ErrorActionPreference = $savedPref
        if ($LASTEXITCODE -eq 0) { Remove-Item $batPath -Force -ErrorAction SilentlyContinue }
    }
}

function Test-SubmoduleCheckedOut([string]$Path, [string]$ExpectedTag) {
    if (-not (Test-Path (Join-Path $Path '.git'))) {
        throw "Submodule not initialized: $Path. Run 'git submodule update --init --recursive' first."
    }
    Push-Location $Path
    try {
        $describe = & git describe --tags --exact-match HEAD 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $describe) {
            Write-Host "  WARN: $Path is not on tag $ExpectedTag (HEAD detached but no exact tag match)" -ForegroundColor Yellow
        } elseif ($describe.Trim() -ne $ExpectedTag) {
            Write-Host "  WARN: $Path is on tag '$($describe.Trim())', expected '$ExpectedTag'" -ForegroundColor Yellow
        } else {
            Write-Step "$Path @ $ExpectedTag"
        }
    } finally {
        Pop-Location
    }
}

function Copy-IfDifferent([string]$Source, [string]$Dest) {
    $destDir = Split-Path -Parent $Dest
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
    Copy-Item -Path $Source -Destination $Dest -Force
    Write-OK "  -> $($Dest.Substring($projectRoot.Length + 1))"
}

# ============================================================================
# Environment discovery
# ============================================================================
Write-Section 'Environment'

$vsInstall = Find-VsInstall
$vsDevCmd  = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $vsDevCmd)) { throw "VsDevCmd.bat not found at $vsDevCmd" }
Write-Step "VS install   : $vsInstall"

$cmakeExe = Find-Tool 'cmake.exe' @(
    (Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
)
if (-not $cmakeExe) { throw "cmake.exe not found." }
Write-Step "cmake        : $cmakeExe"

$ninjaExe = Find-Tool 'ninja.exe' @(
    (Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')
)
if (-not $ninjaExe) { throw "ninja.exe not found." }
Write-Step "ninja        : $ninjaExe"

# Strawberry Perl is required: OpenSSL Configure needs CORE modules
# (Locale::Maketext::Simple, Params::Check, IPC::Cmd) that Git for Windows'
# stripped-down perl does not ship. Prefer explicit Strawberry locations over
# whatever 'perl.exe' is on PATH first (which would resolve to Git's perl).
$perlCandidates = @(
    'C:\Strawberry\perl\bin\perl.exe'
    (Join-Path $env:ProgramFiles 'Strawberry\perl\bin\perl.exe')
)
$perlExe = $perlCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $perlExe) {
    throw "Strawberry Perl not found. Install via 'winget install StrawberryPerl.StrawberryPerl'."
}
Write-Step "perl         : $perlExe"

$nasmExe = Find-Tool 'nasm.exe' @(
    (Join-Path $env:LOCALAPPDATA 'bin\NASM\nasm.exe'),
    (Join-Path $env:ProgramFiles 'NASM\nasm.exe')
)
if (-not $nasmExe) {
    throw "nasm.exe not found. Install via 'winget install NASM.NASM' (OpenSSL 3.x needs it for both x64 and x86)."
}
Write-Step "nasm         : $nasmExe"

# jom is Qt's parallel nmake replacement. Optional: fall back to nmake if not
# present, but with 24 logical cores parallel OpenSSL compile is ~10x faster.
$jomExe = Find-Tool 'jom.exe' @(
    'C:\Qt\Tools\QtCreator\bin\jom\jom.exe'
    (Join-Path $env:ProgramFiles      'Qt\Tools\QtCreator\bin\jom\jom.exe')
    (Join-Path ${env:ProgramFiles(x86)} 'Qt\Tools\QtCreator\bin\jom\jom.exe')
)
if ($jomExe) {
    Write-Step "jom          : $jomExe"
} else {
    Write-Step 'jom          : not found (will fall back to single-threaded nmake)'
}

# ============================================================================
# Submodule sanity
# ============================================================================
Write-Section 'Submodules'
Test-SubmoduleCheckedOut $opensslSrc $expectedOpensslTag
Test-SubmoduleCheckedOut $libssh2Src $expectedLibssh2Tag

# ============================================================================
# Clean
# ============================================================================
if ($Clean -and (Test-Path $buildRoot)) {
    Write-Section 'Clean'
    Write-Step "Removing $buildRoot"
    Remove-Item -Recurse -Force $buildRoot
}
if (-not (Test-Path $buildRoot)) {
    New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
}

# ============================================================================
# OpenSSL build
# ============================================================================
function Build-OpenSSL {
    param(
        [Parameter(Mandatory)] [ValidateSet('x64','x86')] [string]$Arch,
        [Parameter(Mandatory)] [string]$OutDir
    )

    Write-Section "OpenSSL $Arch"

    # OpenSSL builds in-source. Wipe any previous in-tree build before reconfiguring.
    if (Test-Path (Join-Path $opensslSrc 'Makefile')) {
        $cleanBody = @"
set "PATH=$vswhereDir;%PATH%"
pushd "$opensslSrc"
call "$vsDevCmd" -arch=$Arch -host_arch=x64 -no_logo
nmake -nologo distclean
popd
exit /b 0
"@
        Invoke-CmdScript -Body $cleanBody -Description "OpenSSL ${Arch}: distclean prior in-tree build"
    }
    Push-Location $opensslSrc
    try {
        # Defensive: force-remove any stragglers if distclean was unavailable.
        Get-ChildItem -Filter '*.obj' -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    } finally {
        Pop-Location
    }

    if (Test-Path $OutDir) { Remove-Item -Recurse -Force $OutDir }
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

    $vcArch  = if ($Arch -eq 'x64') { 'VC-WIN64A' } else { 'VC-WIN32' }
    # OpenSSL 3.x needs NASM for both x64 and x86 assembly. Without it Configure
    # aborts with "Failure! build file wasn't produced. NASM not found".
    # NOTE: $nasmDir is referenced by the cmd here-string at the bottom of this
    # function; PSScriptAnalyzer can't see that and warns falsely.
    [Diagnostics.CodeAnalysis.SuppressMessageAttribute('PSUseDeclaredVarsMoreThanAssignments', '')]
    $nasmDir = Split-Path -Parent $nasmExe

    # Configure flags chosen for libssh2's needs only:
    #   no-shared      : static .lib only, no DLLs
    #   no-tests/docs/apps : drop binaries we never ship
    #   no-deprecated  : strip pre-1.1.0 API surface
    #   no-engine/dso  : remove dynamic provider loading machinery
    #   no-legacy      : skip legacy provider (MD2/RC5/etc.)
    #   no-ssl/tls/dtls : libssh2 only needs libcrypto, never the TLS stack
    #   no-fips        : avoid FIPS provider build
    #   /MT            : static C runtime (matches plugin link mode)
    $configureArgs = @(
        $vcArch
        '/MT'
        # /FS serializes PDB writes inside cl.exe so parallel jom workers can
        # share the single ossl_static.pdb without "C1041" PDB-locking errors.
        '/FS'
        'no-shared'
        'no-tests'
        'no-docs'
        'no-apps'
        'no-deprecated'
        'no-engine'
        'no-dso'
        'no-legacy'
        'no-ssl'
        'no-tls'
        'no-dtls'
        'no-fips'
        "--prefix=$OutDir"
        "--openssldir=$OutDir\ssl"
    ) -join ' '

    # Each line below is a separate cmd statement; an early failure stops the
    # batch via 'if errorlevel 1 exit /b'. $vswhereDir is added to PATH so that
    # VsDevCmd's internal probes do not log "vswhere.exe is not recognized".

    # Use jom for parallel build when available; nmake is single-threaded.
    if ($jomExe) {
        $cores      = [Environment]::ProcessorCount
        $buildCmd   = "`"$jomExe`" -j $cores build_libs"
        $installCmd = "`"$jomExe`" -j $cores install_dev"
    } else {
        $buildCmd   = 'nmake -nologo build_libs'
        $installCmd = 'nmake -nologo install_dev'
    }

    $body = @"
set "PATH=$nasmDir;$vswhereDir;%PATH%"
pushd "$opensslSrc"
call "$vsDevCmd" -arch=$Arch -host_arch=x64 -no_logo
if errorlevel 1 exit /b 1
"$perlExe" Configure $configureArgs
if errorlevel 1 exit /b 1
$buildCmd
if errorlevel 1 exit /b 1
$installCmd
if errorlevel 1 exit /b 1
popd
exit /b 0
"@

    Invoke-CmdScript -Body $body -Description "OpenSSL ${Arch}: Configure + build + install"

    # Verify install layout matches what libssh2 (and our copy step) expects.
    $crypto = Join-Path $OutDir 'lib\libcrypto.lib'
    if (-not (Test-Path $crypto)) { throw "OpenSSL $Arch built but libcrypto.lib not found at $crypto" }
    Write-OK "libcrypto.lib ($Arch) ready"
}

if (-not $SkipOpenSSL) {
    Build-OpenSSL -Arch x64 -OutDir $opensslOutX64
    if (-not $X64Only) {
        Build-OpenSSL -Arch x86 -OutDir $opensslOutX86
    }
}

# ============================================================================
# libssh2 build
# ============================================================================
function Build-Libssh2 {
    param(
        [Parameter(Mandatory)] [ValidateSet('x64','x86')] [string]$Arch,
        [Parameter(Mandatory)] [string]$BuildDir,
        [Parameter(Mandatory)] [string]$OpenSSLPrefix
    )

    Write-Section "libssh2 $Arch"

    if (-not (Test-Path (Join-Path $OpenSSLPrefix 'lib\libcrypto.lib'))) {
        throw "OpenSSL $Arch not found at $OpenSSLPrefix. Run without -SkipOpenSSL first."
    }

    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

    # CMake flags map to libssh2's CMakeLists.txt options:
    #   CRYPTO_BACKEND=OpenSSL              -> use libcrypto (not WinCNG/mbedTLS)
    #   BUILD_SHARED_LIBS=OFF               -> static libssh2.lib
    #   BUILD_EXAMPLES/TESTING=OFF          -> skip everything we don't ship
    #   ENABLE_ZLIB_COMPRESSION=OFF         -> zlib not available, plugin doesn't need it
    #   OPENSSL_USE_STATIC_LIBS=TRUE        -> link our static libcrypto, not a system DLL
    #   CMAKE_MSVC_RUNTIME_LIBRARY=Multi... -> /MT to match the plugin
    $cmakeArgs = @(
        '-S', $libssh2Src
        '-B', $BuildDir
        '-G', 'Ninja'
        "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
        '-DCMAKE_BUILD_TYPE=Release'
        # libssh2 1.11.1's cmake_minimum_required(3.10) leaves CMP0091 in OLD
        # mode, which makes CMAKE_MSVC_RUNTIME_LIBRARY a no-op. Force-enable the
        # policy so our /MT request actually applies; without this the .obj
        # files leak /MD CRT imports (LNK2001 __imp_difftime64 / __imp_rewind).
        '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW'
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded'
        '-DCRYPTO_BACKEND=OpenSSL'
        '-DBUILD_SHARED_LIBS=OFF'
        '-DBUILD_EXAMPLES=OFF'
        '-DBUILD_TESTING=OFF'
        '-DENABLE_ZLIB_COMPRESSION=OFF'
        '-DOPENSSL_USE_STATIC_LIBS=TRUE'
        "-DOPENSSL_ROOT_DIR=$OpenSSLPrefix"
    )

    # CMake's MSVC generator detection needs cl.exe + ninja on PATH; activate VsDevCmd
    # for the right architecture, then invoke cmake.
    $cmakeArgsQuoted = ($cmakeArgs | ForEach-Object { "`"$_`"" }) -join ' '
    $configureBody = @"
set "PATH=$vswhereDir;%PATH%"
call "$vsDevCmd" -arch=$Arch -host_arch=x64 -no_logo
if errorlevel 1 exit /b 1
"$cmakeExe" $cmakeArgsQuoted
exit /b %errorlevel%
"@
    Invoke-CmdScript -Body $configureBody -Description "libssh2 ${Arch}: cmake configure"

    $buildBody = @"
set "PATH=$vswhereDir;%PATH%"
call "$vsDevCmd" -arch=$Arch -host_arch=x64 -no_logo
if errorlevel 1 exit /b 1
"$cmakeExe" --build "$BuildDir" --config Release
exit /b %errorlevel%
"@
    Invoke-CmdScript -Body $buildBody -Description "libssh2 ${Arch}: cmake build"

    # Locate produced lib (Ninja places it under src/ in the build dir).
    $candidate = Get-ChildItem -Path $BuildDir -Filter 'libssh2.lib' -Recurse -ErrorAction SilentlyContinue |
                 Select-Object -First 1
    if (-not $candidate) { throw "libssh2.lib not produced under $BuildDir" }
    Write-OK "libssh2.lib ($Arch) at $($candidate.FullName)"
    return $candidate.FullName
}

$libssh2LibX64 = $null
$libssh2LibX86 = $null
if (-not $SkipLibssh2) {
    $libssh2LibX64 = Build-Libssh2 -Arch x64 -BuildDir $libssh2BuildX64 -OpenSSLPrefix $opensslOutX64
    if (-not $X64Only) {
        $libssh2LibX86 = Build-Libssh2 -Arch x86 -BuildDir $libssh2BuildX86 -OpenSSLPrefix $opensslOutX86
    }
}

# ============================================================================
# Install: copy libs + headers into src/ tree
# ============================================================================
Write-Section 'Install into src/'

if (-not (Test-Path $libDestDir))     { New-Item -ItemType Directory -Path $libDestDir -Force | Out-Null }
if (-not (Test-Path $includeDestDir)) { New-Item -ItemType Directory -Path $includeDestDir -Force | Out-Null }

# OpenSSL libcrypto -> src/lib/libcrypto_<arch>.lib
if (-not $SkipOpenSSL) {
    Copy-IfDifferent (Join-Path $opensslOutX64 'lib\libcrypto.lib') (Join-Path $libDestDir 'libcrypto_x64.lib')
    if (-not $X64Only) {
        Copy-IfDifferent (Join-Path $opensslOutX86 'lib\libcrypto.lib') (Join-Path $libDestDir 'libcrypto_x86.lib')
    }
}

# libssh2 -> src/lib/libssh2_<arch>.lib
if (-not $SkipLibssh2) {
    if ($libssh2LibX64) { Copy-IfDifferent $libssh2LibX64 (Join-Path $libDestDir 'libssh2_x64.lib') }
    if ($libssh2LibX86) { Copy-IfDifferent $libssh2LibX86 (Join-Path $libDestDir 'libssh2_x86.lib') }
}

# libssh2 public headers -> src/include/libssh2/
# (libssh2_config.h is a build artifact, not exposed to consumers; we skip it.)
$headerSources = @(
    'libssh2.h'
    'libssh2_sftp.h'
    'libssh2_publickey.h'
)
foreach ($h in $headerSources) {
    $src = Join-Path $libssh2Src "include\$h"
    if (Test-Path $src) {
        Copy-IfDifferent $src (Join-Path $includeDestDir $h)
    } else {
        Write-Host "  WARN: header missing in submodule: $src" -ForegroundColor Yellow
    }
}

# ============================================================================
# Summary
# ============================================================================
Write-Section 'Summary'
Get-ChildItem -Path $libDestDir -Filter '*.lib' | ForEach-Object {
    $sizeKB = [math]::Round($_.Length / 1KB, 0)
    Write-Host ("  {0,-30} {1,7} KB" -f $_.Name, $sizeKB) -ForegroundColor Gray
}
Write-Host ''
Write-Host 'Done. Run .\build.ps1 to build the plugin against the produced libs.' -ForegroundColor Cyan
