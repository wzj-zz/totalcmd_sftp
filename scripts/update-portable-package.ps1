param(
    [Parameter(Mandatory)] [string]$ArchivePath,
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'

function Assert-File([string]$Path, [string]$Message) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw $Message }
}

function Assert-Directory([string]$Path, [string]$Message) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { throw $Message }
}

function Assert-ZipEntries([string]$Path, [string[]]$RequiredEntries) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entries = @($zip.Entries | ForEach-Object { $_.FullName })
        foreach ($entry in $RequiredEntries) {
            if ($entries -notcontains $entry) { throw "Updated archive is missing: $entry" }
        }
    } finally {
        $zip.Dispose()
    }
}

function Copy-ZipEntry($Source, $Destination, [string]$Name) {
    $entry = $Destination.CreateEntry($Name, [System.IO.Compression.CompressionLevel]::Optimal)
    $entry.LastWriteTime = $Source.LastWriteTime
    $entry.ExternalAttributes = $Source.ExternalAttributes
    $input = $Source.Open()
    $output = $entry.Open()
    try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
}

function Add-ZipDirectory([System.IO.Compression.ZipArchive]$Archive, [string]$Directory, [string]$Prefix) {
    foreach ($file in Get-ChildItem -LiteralPath $Directory -File -Recurse -Force) {
        $relative = [System.IO.Path]::GetRelativePath($Directory, $file.FullName).Replace('\', '/')
        $entry = $Archive.CreateEntry("$Prefix/$relative", [System.IO.Compression.CompressionLevel]::Optimal)
        $input = [System.IO.File]::OpenRead($file.FullName)
        $output = $entry.Open()
        try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
    }
}

function Replace-PortablePluginEntries([string]$SourceArchive, [string]$ReplacementArchive, [string]$ExtractionRoot, [string]$PortableRoot, [string]$PluginDirectory) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $portablePrefix = ([System.IO.Path]::GetRelativePath($ExtractionRoot, $PortableRoot).Replace('\', '/').TrimEnd('/') + '/')
    $pluginPrefix = "${portablePrefix}Plugins/Wfx/SFTP/"
    $updatedFiles = @("${portablePrefix}Wincmd.ini", "${portablePrefix}usercmd.ini")
    $source = [System.IO.Compression.ZipFile]::OpenRead($SourceArchive)
    $destination = [System.IO.Compression.ZipFile]::Open($ReplacementArchive, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($entry in $source.Entries) {
            if ($entry.FullName.StartsWith($pluginPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or $updatedFiles -contains $entry.FullName) { continue }
            Copy-ZipEntry $entry $destination $entry.FullName
        }
        Add-ZipDirectory $destination $PluginDirectory ($pluginPrefix.TrimEnd('/'))
        foreach ($name in @('Wincmd.ini', 'usercmd.ini')) {
            $file = Join-Path $PortableRoot $name
            $entry = $destination.CreateEntry("${portablePrefix}$name", [System.IO.Compression.CompressionLevel]::Optimal)
            $input = [System.IO.File]::OpenRead($file)
            $output = $entry.Open()
            try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
        }
    } finally {
        $destination.Dispose()
        $source.Dispose()
    }
}

function Find-PortableRoot([string]$ExtractionRoot) {
    $executables = @(Get-ChildItem -LiteralPath $ExtractionRoot -Filter 'Totalcmd64.exe' -File -Recurse)
    if ($executables.Count -ne 1) { throw "Expected exactly one Totalcmd64.exe in the archive, found $($executables.Count)." }
    $root = $executables[0].Directory.FullName
    Assert-File (Join-Path $root 'Wincmd.ini') 'Portable package is missing Wincmd.ini beside Totalcmd64.exe.'
    Assert-File (Join-Path $root 'usercmd.ini') 'Portable package is missing usercmd.ini beside Totalcmd64.exe.'
    return $root
}

function Assert-InitializedPortableRoot([string]$Root) {
    $plugin = Join-Path $Root 'Plugins\Wfx\SFTP'
    foreach ($name in @('SFTPplug.wfx64', 'SftpArchiveRouter.exe', 'SFTPplug.chm', 'sftp.php', '7z.exe', '7z.dll', '7zip-License.txt')) {
        Assert-File (Join-Path $plugin $name) "Updated portable package is missing plugin file: $name"
    }

    $wincmd = [System.IO.File]::ReadAllText((Join-Path $Root 'Wincmd.ini'))
    $usercmd = [System.IO.File]::ReadAllText((Join-Path $Root 'usercmd.ini'))
    foreach ($entry in @(
        'A+F5=em_SftpArchivePack',
        'A+F6=em_SftpArchiveUnpack',
        'A+F7=em_SftpTarCopy',
        'A+F8=em_SftpTarMove',
        'A+F9=em_SftpRemoteDelete',
        'A+F11=em_SftpPrewarmManifest',
        'A+F12=em_SftpLocalDiff',
        'C+G=em_SftpOpenTerminal',
        'CAS+G=em_SftpOpenWindowsTerminalTab',
        'AS+G=em_SftpSplitWindowsTerminal',
        'C+P=em_SftpManageTunnels'
    )) {
        if ($wincmd -notmatch ('(?m)^' + [regex]::Escape($entry) + '\s*$')) { throw "Portable package shortcut is missing: $entry" }
    }
    foreach ($name in @('em_SftpArchivePack', 'em_SftpArchiveUnpack', 'em_SftpTarCopy', 'em_SftpTarMove',
                        'em_SftpRemoteDelete', 'em_SftpPrewarmManifest', 'em_SftpLocalDiff',
                        'em_SftpOpenTerminal', 'em_SftpOpenWindowsTerminalTab',
                        'em_SftpSplitWindowsTerminal', 'em_SftpManageTunnels')) {
        if ($usercmd -notmatch ('(?m)^\[' + [regex]::Escape($name) + '\]\s*$')) { throw "Portable package user command is missing: $name" }
    }
}

$ArchivePath = [System.IO.Path]::GetFullPath($ArchivePath)
Assert-File $ArchivePath 'Portable Total Commander archive is missing.'
Assert-Directory (Split-Path -Parent $ArchivePath) 'Portable archive parent directory is missing.'

$running = @(Get-Process TOTALCMD64, SftpArchiveRouter -ErrorAction SilentlyContinue)
if ($running.Count -ne 0) {
    throw 'Close all Total Commander and SftpArchiveRouter processes before updating a portable package.'
}

$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
Assert-File $msbuild 'The configured Visual Studio MSBuild executable is missing.'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Assert-Directory $repoRoot 'The repository root is missing.'

$runId = [Guid]::NewGuid().ToString('N')
$workRoot = Join-Path ([System.IO.Path]::GetTempPath()) "SftpPortablePackage-$runId"
$replacement = "$ArchivePath.$runId.tmp.zip"
$backup = "$ArchivePath.$runId.bak"

try {
    New-Item -ItemType Directory -Path $workRoot -Force | Out-Null
    & $msbuild (Join-Path $repoRoot 'build\SFTPplug.sln') /m /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }

    $extractionRoot = Join-Path $workRoot 'extracted'
    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $extractionRoot -Force
    $portableRoot = Find-PortableRoot $extractionRoot
    $package = Join-Path $repoRoot 'SFTP'
    Assert-Directory $package 'Generated SFTP deployment package is missing.'
    $pluginTarget = Join-Path $portableRoot 'Plugins\Wfx\SFTP'
    Assert-Directory (Split-Path -Parent $pluginTarget) 'Portable package WFX directory is missing.'
    if (Test-Path -LiteralPath $pluginTarget) { Remove-Item -LiteralPath $pluginTarget -Recurse -Force }
    Copy-Item -LiteralPath $package -Destination $pluginTarget -Recurse -Force

    $router = Join-Path $pluginTarget 'SftpArchiveRouter.exe'
    $init = Start-Process -FilePath $router -ArgumentList @('init', '-y') -Wait -PassThru -NoNewWindow
    if ($init.ExitCode -ne 0) { throw "Router init -y failed with exit code $($init.ExitCode)." }
    Assert-InitializedPortableRoot $portableRoot

    Replace-PortablePluginEntries $ArchivePath $replacement $extractionRoot $portableRoot $pluginTarget
    $portableName = Split-Path -Leaf $portableRoot
    Assert-ZipEntries $replacement @(
        "$portableName/Totalcmd64.exe",
        "$portableName/Plugins/Wfx/SFTP/SFTPplug.wfx64",
        "$portableName/Plugins/Wfx/SFTP/SftpArchiveRouter.exe",
        "$portableName/Plugins/Wfx/SFTP/7z.exe",
        "$portableName/Plugins/Wfx/SFTP/7z.dll",
        "$portableName/Plugins/Wfx/SFTP/7zip-License.txt",
        "$portableName/Wincmd.ini",
        "$portableName/usercmd.ini"
    )
    [System.IO.File]::Replace($replacement, $ArchivePath, $backup)
    $replacement = $null
    Remove-Item -LiteralPath $backup -Force
    $backup = $null
    Write-Host "Portable package updated: $ArchivePath" -ForegroundColor Green
} finally {
    if ($replacement -and (Test-Path -LiteralPath $replacement)) {
        Remove-Item -LiteralPath $replacement -Force -ErrorAction SilentlyContinue
    }
    if ($backup -and (Test-Path -LiteralPath $backup)) {
        Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
    }
    if (-not $KeepArtifacts) {
        Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Host "Portable package work files retained: $workRoot" -ForegroundColor Yellow
    }
}
