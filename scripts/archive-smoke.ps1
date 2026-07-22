param(
    [Parameter(Mandatory)] [string]$TotalCommanderPath,
    [Parameter(Mandatory)] [string[]]$Sessions,
    [Parameter(Mandatory)] [string[]]$WslDistros,
    [string]$Fixture = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tests\fixtures\.oh-my-zsh.zip'),
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'

function Add-Result([string]$Name, [string]$Status, [string]$Details = '') {
    $script:Results.Add([pscustomobject]@{ Name = $Name; Status = $Status; Details = $Details }) | Out-Null
    $line = "[$Status] $Name"
    if ($Details) { $line += " - $Details" }
    Write-Host $line -ForegroundColor $(if ($Status -eq 'PASS') { 'Green' } else { 'Red' })
}

function Read-IniFile([string]$Path) {
    $sections = @{}
    $current = $null
    foreach ($rawLine in Get-Content -LiteralPath $Path) {
        $line = $rawLine.Trim()
        if (-not $line -or $line.StartsWith(';') -or $line.StartsWith('#')) { continue }
        if ($line -match '^\[(.+)\]$') {
            $current = $matches[1]
            if (-not $sections.ContainsKey($current)) { $sections[$current] = @{} }
        } elseif ($current -and $line -match '^([^=]+)=(.*)$') {
            $sections[$current][$matches[1].Trim().ToLowerInvariant()] = $matches[2].Trim()
        }
    }
    return $sections
}

function Get-SshSession([string]$Name, [hashtable]$Section, [int]$Index) {
    if (-not $Section['server'] -or -not $Section['user']) { throw "remote session #$Index has no server or user" }
    $hostName = $Section['server']
    $port = 22
    if ($hostName -match '^\[(.+)\]:(\d+)$') { $hostName = $matches[1]; $port = [int]$matches[2] }
    elseif ($hostName -match '^([^:]+):(\d+)$') { $hostName = $matches[1]; $port = [int]$matches[2] }
    elseif ($Section['customport'] -match '^\d+$') { $port = [int]$Section['customport'] }
    return [pscustomobject]@{
        Label = "remote session #$Index"
        Target = "$($Section['user'])@$hostName"
        SshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20', '-p', [string]$port)
    }
}

function Quote-Remote([string]$Value) {
    return "'" + $Value.Replace("'", "'`"'`"'") + "'"
}

function Invoke-Remote($Session, [string]$Command) {
    $null = & ssh @($Session.SshArgs) $Session.Target $Command 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$($Session.Label): ssh command failed ($LASTEXITCODE)" }
}

function Test-RemotePath($Session, [string]$Path) {
    & ssh @($Session.SshArgs) $Session.Target "test -e $(Quote-Remote $Path)" 2>$null
    return $LASTEXITCODE -eq 0
}

function New-SelectedList([string]$Name, [string]$SelectedPath) {
    $path = Join-Path $script:WorkRoot "$Name.lst"
    [System.IO.File]::WriteAllText($path, "$SelectedPath`r`n", [System.Text.UTF8Encoding]::new($false))
    return $path
}

function Invoke-Router([string]$Operation, [string]$Source, [string]$Target, [string]$Selected, [string]$Name) {
    $list = New-SelectedList $Name $Selected
    $errorFile = Join-Path $script:WorkRoot "$Name.err"
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $script:Router
    $info.UseShellExecute = $false
    $info.RedirectStandardError = $true
    foreach ($argument in @('selftest-operation', $Operation, $Source, $Target, $list, $errorFile)) { [void]$info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    $standardError = $process.StandardError.ReadToEnd()
    if ($process.ExitCode -ne 0) {
        $error = if (Test-Path -LiteralPath $errorFile) { Get-Content -LiteralPath $errorFile -Raw } else { '' }
        if (-not $error) { $error = $standardError }
        throw "router $Operation failed: $($error.Trim())"
    }
}

function Invoke-RouterUnpack([string]$Archive, [string]$Target, [string]$Name) {
    $errorFile = Join-Path $script:WorkRoot "$Name.err"
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $script:Router
    $info.UseShellExecute = $false
    $info.RedirectStandardError = $true
    foreach ($argument in @('selftest-unpack-local', $Archive, $Target, $errorFile)) { [void]$info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    $standardError = $process.StandardError.ReadToEnd()
    if ($process.ExitCode -ne 0) {
        $error = if (Test-Path -LiteralPath $errorFile) { Get-Content -LiteralPath $errorFile -Raw } else { '' }
        if (-not $error) { $error = $standardError }
        throw "router local unpack failed: $($error.Trim())"
    }
}

function Assert-RouterFails([string]$Operation, [string]$Source, [string]$Target, [string]$Selected, [string]$Name) {
    try {
        Invoke-Router $Operation $Source $Target $Selected $Name
    } catch {
        return
    }
    throw 'router operation unexpectedly succeeded'
}

function Wait-ForSession([string]$Path, [int]$Index) {
    $errorFile = Join-Path $script:WorkRoot "session-$Index.err"
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    do {
        Remove-Item -LiteralPath $errorFile -Force -ErrorAction SilentlyContinue
        $process = Start-Process -FilePath $script:Router -ArgumentList @('selftest-session', $Path, $errorFile) -Wait -PassThru
        if ($process.ExitCode -eq 0) { return }
        Start-Sleep -Milliseconds 750
    } while ([DateTime]::UtcNow -lt $deadline)
    $error = if (Test-Path -LiteralPath $errorFile) { Get-Content -LiteralPath $errorFile -Raw } else { '' }
    throw "remote session #$Index did not become active: $($error.Trim())"
}

function Start-TotalCommanderSessions([string]$Executable, [string]$LeftPath, [string]$RightPath) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.UseShellExecute = $false
    foreach ($argument in @('/O', "/L=$LeftPath", "/R=$RightPath")) { [void]$info.ArgumentList.Add($argument) }
    [System.Diagnostics.Process]::Start($info).Dispose()
}

function Remove-UncArtifacts([string[]]$Distros, [string]$RunId) {
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if (-not $wsl) { return }
    foreach ($distro in $Distros) {
        try { & $wsl.Source -d $distro -- rm -rf "/tmp/SftpArchiveSmoke-$RunId" 2>$null } catch {}
    }
}

function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "expected file missing" }
    if ([System.IO.File]::ReadAllText($Path).Trim() -ne 'archive-smoke') { throw "unexpected file content" }
}

function Assert-ExtractedContent([string]$Path) {
    if (@(Get-ChildItem -LiteralPath $Path -Recurse -File -Force).Count -eq 0) { throw 'no files were extracted' }
}

function Assert-RemoteFile($Session, [string]$Path) {
    Invoke-Remote $Session "grep -qx archive-smoke $(Quote-Remote $Path)"
}

function Run-Case([string]$Name, [scriptblock]$Action) {
    try { & $Action; Add-Result $Name 'PASS' } catch { Add-Result $Name 'FAIL' $_.Exception.Message }
}

function Split-Names([string[]]$Names) {
    return @($Names | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

$Sessions = Split-Names $Sessions
$WslDistros = Split-Names $WslDistros
if ($Sessions.Count -ne 2) { throw 'Pass exactly two SFTP session display names with -Sessions.' }
if ($WslDistros.Count -lt 1) { throw 'Pass at least one WSL distribution name with -WslDistros.' }

$TotalCommanderPath = [System.IO.Path]::GetFullPath($TotalCommanderPath)
$Fixture = [System.IO.Path]::GetFullPath($Fixture)
$script:Results = [System.Collections.Generic.List[object]]::new()
$script:RunId = [Guid]::NewGuid().ToString('N').Substring(0, 12)
$script:WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) "SftpArchiveSmoke-$script:RunId"
$plugin = Join-Path $TotalCommanderPath 'Plugins\Wfx\SFTP'
$script:Router = Join-Path $plugin 'SftpArchiveRouter.exe'
$ini = Join-Path $TotalCommanderPath 'sftpplug.ini'

try {
    foreach ($path in @($Fixture, $script:Router, (Join-Path $plugin 'SFTPplug.wfx64'), (Join-Path $plugin '7z.exe'), (Join-Path $plugin '7z.dll'), $ini)) {
        if (-not (Test-Path -LiteralPath $path)) { throw 'required deployed runtime or configuration is missing' }
    }
    $iniData = Read-IniFile $ini
    $remote = @()
    for ($index = 0; $index -lt 2; $index++) {
        if (-not $iniData.ContainsKey($Sessions[$index])) { throw "remote session #$($index + 1) is not in sftpplug.ini" }
        $remote += Get-SshSession $Sessions[$index] $iniData[$Sessions[$index]] ($index + 1)
    }
    $uncTargets = @()
    for ($index = 0; $index -lt $WslDistros.Count; $index++) {
        $wslTmp = Join-Path (Join-Path '\\wsl.localhost' $WslDistros[$index]) 'tmp'
        if (-not (Test-Path -LiteralPath $wslTmp -PathType Container)) { throw "WSL UNC target #$($index + 1) is unavailable" }
        $uncTargets += Join-Path $wslTmp "SftpArchiveSmoke-$script:RunId"
    }
    New-Item -ItemType Directory -Path $script:WorkRoot -Force | Out-Null

    $r1 = "/tmp/sftpplug-archive-smoke-$script:RunId"
    $r2 = "/tmp/sftpplug-archive-smoke-$script:RunId"
    $sftp1 = "\\SFTP\$($Sessions[0])\tmp\sftpplug-archive-smoke-$script:RunId"
    $sftp2 = "\\SFTP\$($Sessions[1])\tmp\sftpplug-archive-smoke-$script:RunId"
    $tcSftp1 = "\\\SFTP\$($Sessions[0])\tmp\sftpplug-archive-smoke-$script:RunId"
    $tcSftp2 = "\\\SFTP\$($Sessions[1])\tmp\sftpplug-archive-smoke-$script:RunId"
    $local = Join-Path $script:WorkRoot 'local'
    New-Item -ItemType Directory -Path $local -Force | Out-Null
    foreach ($uncTarget in $uncTargets) { New-Item -ItemType Directory -Path $uncTarget -Force | Out-Null }
    New-Item -ItemType Directory -Path (Join-Path $local 'payload\nested') -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $local 'payload\nested\value.txt'), 'archive-smoke')

    Run-Case 'bundled 7-Zip fixture -> local directory' {
        $target = Join-Path $local 'fixture-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-RouterUnpack $Fixture $target 'fixture-local'
        Assert-ExtractedContent $target
    }
    for ($index = 0; $index -lt $uncTargets.Count; $index++) {
        $uncTarget = $uncTargets[$index]
        Run-Case "bundled 7-Zip fixture -> WSL UNC target #$($index + 1)" {
            $target = Join-Path $uncTarget 'fixture'; New-Item -ItemType Directory -Path $target -Force | Out-Null
            Invoke-RouterUnpack $Fixture $target ("fixture-unc-{0}" -f ($index + 1))
            Assert-ExtractedContent $target
        }
    }

    foreach ($session in $remote) {
        Invoke-Remote $session "rm -rf $(Quote-Remote $r1); mkdir -p $(Quote-Remote "$r1/source/payload/nested") $(Quote-Remote "$r1/f5") $(Quote-Remote "$r1/f6") $(Quote-Remote "$r1/f7") $(Quote-Remote "$r1/f8"); printf archive-smoke > $(Quote-Remote "$r1/source/payload/nested/value.txt")"
    }

    $tc = Join-Path $TotalCommanderPath 'TOTALCMD64.EXE'
    Start-TotalCommanderSessions $tc $tcSftp1 $tcSftp2
    Wait-ForSession "$sftp1\source" 1
    Wait-ForSession "$sftp2\source" 2
    Add-Result 'active WFX session #1 and #2' 'PASS'

    Run-Case 'Alt+F5 local -> remote archive' {
        Invoke-Router pack $local "$sftp1\f5\local.tar" (Join-Path $local 'payload') 'f5-local'
        Invoke-Remote $remote[0] "tar -tf $(Quote-Remote "$r1/f5/local.tar") | grep -q payload/nested/value.txt"
    }
    Run-Case 'Alt+F5 WSL UNC -> remote archive' {
        $source = Join-Path $uncTargets[0] 'f5-source'; New-Item -ItemType Directory -Path (Join-Path $source 'payload\nested') -Force | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $source 'payload\nested\value.txt'), 'archive-smoke')
        Invoke-Router pack $source "$sftp1\f5\unc.tar" ((Join-Path $source 'payload') + '\') 'f5-unc'
        Invoke-Remote $remote[0] "tar -tf $(Quote-Remote "$r1/f5/unc.tar") | grep -q payload/nested/value.txt"
    }
    Run-Case 'Alt+F5 Unicode WSL UNC -> remote archive' {
        $source = Join-Path $uncTargets[0] 'unicode-测试-source'; $selected = Join-Path $source 'payload-测试\nested'
        New-Item -ItemType Directory -Path $selected -Force | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $selected 'value.txt'), 'archive-smoke')
        Invoke-Router pack $source "$sftp1\f5\unicode-unc.tar" (Split-Path -Parent $selected) 'f5-unicode-unc'
        Invoke-Remote $remote[0] "tar -tf $(Quote-Remote "$r1/f5/unicode-unc.tar") | grep -q '/value.txt'"
    }
    Run-Case 'Alt+F5 remote -> local archive' {
        Invoke-Router pack "$sftp1\source" (Join-Path $local 'f5-remote.tar') "$sftp1\source\payload" 'f5-remote-local'
        if (-not (& "$env:SystemRoot\System32\tar.exe" -tf (Join-Path $local 'f5-remote.tar') | Select-String -Quiet 'payload/nested/value.txt')) { throw 'local archive missing payload' }
    }
    Run-Case 'Alt+F5 remote -> WSL UNC archive' {
        $target = Join-Path $uncTargets[0] 'f5-remote.tar'
        Invoke-Router pack "$sftp1\source" $target "$sftp1\source\payload" 'f5-remote-unc'
        if (-not (& "$env:SystemRoot\System32\tar.exe" -tf $target | Select-String -Quiet 'payload/nested/value.txt')) { throw 'UNC archive missing payload' }
    }
    Run-Case 'Alt+F5 remote #1 -> remote #2 archive' {
        Invoke-Router pack "$sftp1\source" "$sftp2\f5\cross.tar" "$sftp1\source\payload" 'f5-cross'
        Invoke-Remote $remote[1] "tar -tf $(Quote-Remote "$r2/f5/cross.tar") | grep -q payload/nested/value.txt"
    }
    Run-Case 'Alt+F6 local archive -> remote' {
        Invoke-Router unpack $local "$sftp1\f6" (Join-Path $local 'f5-remote.tar') 'f6-local-remote'
        Assert-RemoteFile $remote[0] "$r1/f6/payload/nested/value.txt"
    }
    Run-Case 'Alt+F6 WSL UNC archive -> remote' {
        $archive = Join-Path $uncTargets[0] 'f5-remote.tar'
        Invoke-Router unpack (Split-Path -Parent $archive) "$sftp1\f6" $archive 'f6-unc-remote'
        Assert-RemoteFile $remote[0] "$r1/f6/payload/nested/value.txt"
    }
    Run-Case 'Alt+F6 remote archive -> local' {
        $target = Join-Path $local 'f6-remote-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router unpack "$sftp1\f5" $target "$sftp1\f5\local.tar" 'f6-remote-local'
        Assert-File (Join-Path $target 'payload\nested\value.txt')
    }
    for ($index = 0; $index -lt $uncTargets.Count; $index++) {
        $uncTarget = $uncTargets[$index]
        Run-Case "Alt+F6 remote archive -> WSL UNC target #$($index + 1)" {
            $target = Join-Path $uncTarget 'f6'; New-Item -ItemType Directory -Path $target -Force | Out-Null
            Invoke-Router unpack "$sftp1\f5" $target "$sftp1\f5\local.tar" ("f6-remote-unc-{0}" -f ($index + 1))
            Assert-File (Join-Path $target 'payload\nested\value.txt')
        }
    }
    Run-Case 'Alt+F6 remote #1 archive -> remote #2' {
        Invoke-Router unpack "$sftp1\f5" "$sftp2\f6" "$sftp1\f5\local.tar" 'f6-cross'
        Assert-RemoteFile $remote[1] "$r2/f6/payload/nested/value.txt"
    }
    Run-Case 'Alt+F7 local -> remote' {
        Invoke-Router copy $local "$sftp1\f7" (Join-Path $local 'payload') 'f7-local-remote'
        Assert-RemoteFile $remote[0] "$r1/f7/payload/nested/value.txt"
    }
    for ($index = 0; $index -lt $uncTargets.Count; $index++) {
        $uncTarget = $uncTargets[$index]
        Run-Case "Alt+F7 WSL UNC target #$($index + 1) -> remote" {
            $source = Join-Path $uncTarget 'f7-source'; New-Item -ItemType Directory -Path (Join-Path $source 'payload\nested') -Force | Out-Null
            [System.IO.File]::WriteAllText((Join-Path $source 'payload\nested\value.txt'), 'archive-smoke')
            Invoke-Router copy $source "$sftp1\f7" (Join-Path $source 'payload') ("f7-unc-remote-{0}" -f ($index + 1))
            Assert-RemoteFile $remote[0] "$r1/f7/payload/nested/value.txt"
        }
    }
    Run-Case 'Alt+F7 remote -> local' {
        $target = Join-Path $local 'f7-remote-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router copy "$sftp1\source" $target "$sftp1\source\payload" 'f7-remote-local'
        Assert-File (Join-Path $target 'payload\nested\value.txt')
    }
    for ($index = 0; $index -lt $uncTargets.Count; $index++) {
        $uncTarget = $uncTargets[$index]
        Run-Case "Alt+F7 remote -> WSL UNC target #$($index + 1)" {
            $target = Join-Path $uncTarget 'f7'; New-Item -ItemType Directory -Path $target -Force | Out-Null
            Invoke-Router copy "$sftp1\source" $target "$sftp1\source\payload" ("f7-remote-unc-{0}" -f ($index + 1))
            Assert-File (Join-Path $target 'payload\nested\value.txt')
        }
    }
    Run-Case 'Alt+F7 remote #1 -> remote #2' {
        Invoke-Router copy "$sftp1\source" "$sftp2\f7" "$sftp1\source\payload" 'f7-cross'
        Assert-RemoteFile $remote[1] "$r2/f7/payload/nested/value.txt"
    }
    Run-Case 'Alt+F8 local -> remote deletes source after success' {
        $source = Join-Path $local 'f8-local'; New-Item -ItemType Directory -Path $source -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $local 'payload') -Destination $source -Recurse
        Invoke-Router move $source "$sftp1\f8" (Join-Path $source 'payload') 'f8-local-remote'
        Assert-RemoteFile $remote[0] "$r1/f8/payload/nested/value.txt"
        if (Test-Path -LiteralPath (Join-Path $source 'payload')) { throw 'local source was not deleted' }
    }
    Run-Case 'Alt+F8 WSL UNC -> remote deletes source after success' {
        $source = Join-Path $uncTargets[0] 'f8-source'; New-Item -ItemType Directory -Path (Join-Path $source 'payload\nested') -Force | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $source 'payload\nested\value.txt'), 'archive-smoke')
        Invoke-Router move $source "$sftp1\f8" (Join-Path $source 'payload') 'f8-unc-remote'
        Assert-RemoteFile $remote[0] "$r1/f8/payload/nested/value.txt"
        if (Test-Path -LiteralPath (Join-Path $source 'payload')) { throw 'UNC source was not deleted' }
    }
    Run-Case 'Alt+F8 remote -> local deletes source after success' {
        $target = Join-Path $local 'f8-remote-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router move "$sftp1\source" $target "$sftp1\source\payload" 'f8-remote-local'
        Assert-File (Join-Path $local 'f8-remote-local\payload\nested\value.txt')
        if (Test-RemotePath $remote[0] "$r1/source/payload") { throw 'remote source was not deleted' }
    }
    Run-Case 'Alt+F8 remote -> WSL UNC deletes source after success' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/move-unc/payload/nested"); printf archive-smoke > $(Quote-Remote "$r1/move-unc/payload/nested/value.txt")"
        $target = Join-Path $uncTargets[0] 'f8-remote'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router move "$sftp1\move-unc" $target "$sftp1\move-unc\payload" 'f8-remote-unc'
        Assert-File (Join-Path $target 'payload\nested\value.txt')
        if (Test-RemotePath $remote[0] "$r1/move-unc/payload") { throw 'remote source was not deleted' }
    }
    Run-Case 'Alt+F8 remote #1 -> remote #2 deletes source after success' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/move/payload/nested"); printf archive-smoke > $(Quote-Remote "$r1/move/payload/nested/value.txt")"
        Invoke-Router move "$sftp1\move" "$sftp2\f8" "$sftp1\move\payload" 'f8-cross'
        Assert-RemoteFile $remote[1] "$r2/f8/payload/nested/value.txt"
        if (Test-RemotePath $remote[0] "$r1/move/payload") { throw 'remote source was not deleted' }
    }
    Run-Case 'Alt+F8 failed remote target preserves source' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/failed/payload/nested"); printf archive-smoke > $(Quote-Remote "$r1/failed/payload/nested/value.txt")"
        Assert-RouterFails move "$sftp1\failed" "$sftp2\missing-parent\target" "$sftp1\failed\payload" 'f8-failed-target'
        Assert-RemoteFile $remote[0] "$r1/failed/payload/nested/value.txt"
    }
    Run-Case 'Alt+F9 remote deletion' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/delete/tree"); printf archive-smoke > $(Quote-Remote "$r1/delete/tree/value.txt")"
        Invoke-Router delete "$sftp1\delete" '' "$sftp1\delete\tree" 'f9-delete'
        if (Test-RemotePath $remote[0] "$r1/delete/tree") { throw 'remote tree was not deleted' }
    }
} finally {
    if (Get-Variable remote -Scope Script -ErrorAction SilentlyContinue) { foreach ($session in $remote) { try { Invoke-Remote $session "rm -rf /tmp/sftpplug-archive-smoke-$script:RunId" } catch {} } }
    if (-not $KeepArtifacts) {
        Remove-UncArtifacts $WslDistros $script:RunId
        Remove-Item -LiteralPath $script:WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (@($script:Results | Where-Object Status -eq 'FAIL').Count -gt 0) { exit 1 }
Write-Host 'Archive smoke cleanup complete.' -ForegroundColor Green
exit 0
