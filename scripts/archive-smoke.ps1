param(
    [Parameter(Mandatory)] [string]$TotalCommanderPath,
    [Parameter(Mandatory)] [string[]]$Sessions,
    [Parameter(Mandatory)] [string[]]$WslDistros,
    [Parameter(Mandatory)] [string]$WindowsSession,
    [string]$Fixture = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tests\fixtures\.oh-my-zsh.zip'),
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'

$script:ExternalProcessTimeoutMs = 120000

function Stop-TestProcess([System.Diagnostics.Process]$Process) {
    if (-not $Process) { return }
    try {
        if (-not $Process.HasExited) {
            $Process.Kill($true)
            $Process.WaitForExit(5000)
        }
    } catch [System.InvalidOperationException] {
    } finally {
        $Process.Dispose()
    }
}

function Invoke-ExternalProcess([string]$FileName, [string[]]$Arguments, [int]$TimeoutMs = $script:ExternalProcessTimeoutMs) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $FileName
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($info)
    try {
        $outputTask = $process.StandardOutput.ReadToEndAsync()
        $errorTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMs)) {
            Stop-TestProcess $process
            throw "$FileName timed out after $($TimeoutMs / 1000) seconds."
        }
        [pscustomobject]@{
            ExitCode = $process.ExitCode
            Output = $outputTask.GetAwaiter().GetResult()
            Error = $errorTask.GetAwaiter().GetResult()
        }
    } finally {
        if ($process) { $process.Dispose() }
    }
}

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
    if (-not $Section['server'] -or -not $Section['user']) { throw "Unix remote session #$Index has no server or user" }
    $hostName = $Section['server']
    $port = 22
    if ($hostName -match '^\[(.+)\]:(\d+)$') { $hostName = $matches[1]; $port = [int]$matches[2] }
    elseif ($hostName -match '^([^:]+):(\d+)$') { $hostName = $matches[1]; $port = [int]$matches[2] }
    elseif ($Section['customport'] -match '^\d+$') { $port = [int]$Section['customport'] }
    return [pscustomobject]@{
        Label = "Unix remote session #$Index"
        Target = "$($Section['user'])@$hostName"
        SshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20', '-p', [string]$port)
        ScpArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20', '-o', 'ServerAliveInterval=30', '-o', 'ServerAliveCountMax=4', '-o', 'TCPKeepAlive=yes', '-P', [string]$port)
    }
}

function Quote-Remote([string]$Value) {
    return "'" + $Value.Replace("'", "'`"'`"'") + "'"
}

function Invoke-Remote($Session, [string]$Command) {
    $result = Invoke-ExternalProcess 'ssh.exe' (@($Session.SshArgs) + @($Session.Target, $Command))
    if ($result.ExitCode -ne 0) {
        throw "$($Session.Label): SSH command failed ($($result.ExitCode))."
    }
}

function Copy-LocalFileToRemote($Session, [string]$Source, [string]$Destination) {
    $result = Invoke-ExternalProcess 'scp.exe' (@($Session.ScpArgs) + @('--', $Source, "$($Session.Target):$Destination"))
    if ($result.ExitCode -ne 0) {
        throw "$($Session.Label): fixture copy failed ($($result.ExitCode))."
    }
}

function Expand-PayloadOnUnixRemote($Session, [string]$Archive, [string]$Destination) {
    $remoteArchive = "$Destination/.sftp-archive-smoke-payload.tar"
    Copy-LocalFileToRemote $Session $Archive $remoteArchive
    Invoke-Remote $Session "tar -xf $(Quote-Remote $remoteArchive) -C $(Quote-Remote $Destination) && rm -f $(Quote-Remote $remoteArchive)"
}

function Expand-PayloadOnWindowsRemote($Session, [string]$Archive, [string]$Destination) {
    $remoteArchive = $Destination.TrimEnd('\') + '\.sftp-archive-smoke-payload.tar'
    Copy-LocalFileToRemote $Session $Archive ($remoteArchive.Replace('\', '/'))
    $archiveLiteral = $remoteArchive.Replace("'", "''")
    $destinationLiteral = $Destination.Replace("'", "''")
    Invoke-WindowsRemote $Session "& tar.exe -xf '$archiveLiteral' -C '$destinationLiteral'; if (`$LASTEXITCODE -ne 0) { exit `$LASTEXITCODE }; Remove-Item -LiteralPath '$archiveLiteral' -Force" | Out-Null
}

function ConvertTo-PowerShellEncodedCommand([string]$Script) {
    return [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Script))
}

function Invoke-WindowsRemote($Session, [string]$Script) {
    $command = 'powershell.exe -NoProfile -NonInteractive -EncodedCommand ' + (ConvertTo-PowerShellEncodedCommand $Script)
    $result = Invoke-ExternalProcess 'ssh.exe' (@($Session.SshArgs) + @($Session.Target, $command))
    if ($result.ExitCode -ne 0) {
        throw "$($Session.Label): Windows SSH command failed ($($result.ExitCode))."
    }
    return @($result.Output -split "`r?`n" | Where-Object { $_ })
}

function Test-RemotePath($Session, [string]$Path) {
    $result = Invoke-ExternalProcess 'ssh.exe' (@($Session.SshArgs) + @($Session.Target, "test -e $(Quote-Remote $Path)"))
    return $result.ExitCode -eq 0
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
    $standardErrorTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($script:ExternalProcessTimeoutMs)) {
        Stop-TestProcess $process
        throw "router $Operation timed out after $($script:ExternalProcessTimeoutMs / 1000) seconds."
    }
    $standardError = $standardErrorTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) {
        $error = if (Test-Path -LiteralPath $errorFile) { Get-Content -LiteralPath $errorFile -Raw } else { '' }
        if (-not $error) { $error = $standardError }
        throw "router $Operation failed."
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
    $standardErrorTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($script:ExternalProcessTimeoutMs)) {
        Stop-TestProcess $process
        throw "router local unpack timed out after $($script:ExternalProcessTimeoutMs / 1000) seconds."
    }
    $standardError = $standardErrorTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) {
        $error = if (Test-Path -LiteralPath $errorFile) { Get-Content -LiteralPath $errorFile -Raw } else { '' }
        if (-not $error) { $error = $standardError }
        throw 'router local unpack failed.'
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
    $standardError = ''
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    do {
        Remove-Item -LiteralPath $errorFile -Force -ErrorAction SilentlyContinue
        $sessionInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $sessionInfo.FileName = $script:Router
        $sessionInfo.UseShellExecute = $false
        $sessionInfo.RedirectStandardError = $true
        foreach ($argument in @('selftest-session', $Path, $errorFile)) { [void]$sessionInfo.ArgumentList.Add($argument) }
        $process = [System.Diagnostics.Process]::Start($sessionInfo)
        $standardErrorTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($script:ExternalProcessTimeoutMs)) {
            Stop-TestProcess $process
            throw "Remote WFX session #$Index probe timed out after $($script:ExternalProcessTimeoutMs / 1000) seconds."
        }
        $standardError = $standardErrorTask.GetAwaiter().GetResult()
        if ($process.ExitCode -eq 0) { return }
        Start-Sleep -Milliseconds 750
    } while ([DateTime]::UtcNow -lt $deadline)
    $error = if (Test-Path -LiteralPath $errorFile) { Get-Content -LiteralPath $errorFile -Raw } else { '' }
    if (-not $error) { $error = $standardError }
    throw "Remote WFX session #$Index did not become active."
}

function Invoke-RouterPrewarm([string]$Path, [string]$Name) {
    $errorFile = Join-Path $script:WorkRoot "$Name.err"
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $script:Router
    $info.UseShellExecute = $false
    $info.RedirectStandardError = $true
    foreach ($argument in @('selftest-prewarm', $Path, $errorFile)) { [void]$info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($info)
    $standardErrorTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($script:ExternalProcessTimeoutMs)) {
        Stop-TestProcess $process
        throw "router prewarm timed out after $($script:ExternalProcessTimeoutMs / 1000) seconds."
    }
    $standardError = $standardErrorTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) {
        $error = if (Test-Path -LiteralPath $errorFile) { Get-Content -LiteralPath $errorFile -Raw } else { '' }
        if (-not $error) { $error = $standardError }
        throw 'router prewarm failed.'
    }
}

function Start-TotalCommanderSessions([string]$Executable, [string]$LeftPath, [string]$RightPath) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Executable
    $info.UseShellExecute = $false
    foreach ($argument in @('/O', "/L=$LeftPath", "/R=$RightPath")) { [void]$info.ArgumentList.Add($argument) }
    [System.Diagnostics.Process]::Start($info).Dispose()
}

function Close-TestTotalCommander([string]$Executable) {
    $fullPath = [System.IO.Path]::GetFullPath($Executable)
    $processes = @(Get-Process TOTALCMD64 -ErrorAction SilentlyContinue | Where-Object {
        try { $_.MainModule.FileName -eq $fullPath } catch { $false }
    })
    if ($processes.Count -eq 0) { return }
    Start-Process -FilePath $fullPath -ArgumentList @('/O', '/L=C:\', '/R=C:\') -Wait
    Start-Sleep -Milliseconds 500
    $processes | ForEach-Object { [void]$_.CloseMainWindow() }
    $processes | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
}

function Activate-RemoteSessions([string]$Executable, [string]$LeftPath, [int]$LeftIndex, [string]$RightPath, [int]$RightIndex) {
    for ($attempt = 1; $attempt -le 3; ++$attempt) {
        Start-TotalCommanderSessions $Executable $LeftPath $RightPath
        try {
            Wait-ForSession $LeftPath $LeftIndex
            Wait-ForSession $RightPath $RightIndex
            return
        } catch {
            if ($attempt -eq 3) { throw }
            Start-Sleep -Seconds $attempt
        }
    }
}

function Remove-UncArtifacts([string[]]$Distros, [string]$RunId) {
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if (-not $wsl) { return }
    for ($index = 0; $index -lt $Distros.Count; ++$index) {
        $distro = $Distros[$index]
        Write-Host "[INFO] Removing WSL UNC artifacts for target #$($index + 1)"
        try {
            $result = Invoke-ExternalProcess $wsl.Source @('-d', $distro, '--', 'rm', '-rf', "/tmp/SftpArchiveSmoke-$RunId")
            if ($result.ExitCode -ne 0) { Write-Host "[WARN] WSL cleanup returned exit code $($result.ExitCode)" }
        } catch { Write-Host '[WARN] WSL cleanup failed' }
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

function Assert-WindowsRemoteFile($Session, [string]$Path) {
    $literal = $Path.Replace("'", "''")
    Invoke-WindowsRemote $Session "if (-not (Test-Path -LiteralPath '$literal' -PathType Leaf)) { exit 1 }; if ((Get-Content -LiteralPath '$literal' -Raw).Trim() -ne 'archive-smoke') { exit 1 }" | Out-Null
}

function Test-WindowsRemotePath($Session, [string]$Path) {
    $literal = $Path.Replace("'", "''")
    $command = 'powershell.exe -NoProfile -NonInteractive -EncodedCommand ' + (ConvertTo-PowerShellEncodedCommand "if (Test-Path -LiteralPath '$literal') { exit 0 }; exit 1")
    $result = Invoke-ExternalProcess 'ssh.exe' (@($Session.SshArgs) + @($Session.Target, $command))
    return $result.ExitCode -eq 0
}

function Assert-WindowsRemoteTarEntry($Session, [string]$Path, [string]$Entry) {
    $pathLiteral = $Path.Replace("'", "''")
    $entryLiteral = $Entry.Replace("'", "''")
    Invoke-WindowsRemote $Session "`$entries=@(& tar.exe -tf '$pathLiteral'); if (`$LASTEXITCODE -ne 0 -or `$entries -notcontains '$entryLiteral') { exit 1 }" | Out-Null
}

function Assert-WindowsNoArchiveTemps($Session, [string]$Path) {
    $pathLiteral = $Path.Replace("'", "''")
    Invoke-WindowsRemote $Session "if (@(Get-ChildItem -LiteralPath '$pathLiteral' -Filter '.sftp-archive-*.tmp' -File -Force -ErrorAction SilentlyContinue).Count -ne 0) { exit 1 }" | Out-Null
}

function Run-Case([string]$Name, [scriptblock]$Action) {
    Write-Host "[INFO] Starting $Name"
    try { & $Action; Add-Result $Name 'PASS' } catch { Add-Result $Name 'FAIL' $_.Exception.Message }
}

function Split-Names([string[]]$Names) {
    return @($Names | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

$Sessions = Split-Names $Sessions
$WslDistros = Split-Names $WslDistros
if ($Sessions.Count -ne 2) { throw 'Pass exactly two SFTP session display names with -Sessions.' }
if ($WslDistros.Count -lt 1) { throw 'Pass at least one WSL distribution name with -WslDistros.' }
if (-not $WindowsSession.Trim()) { throw 'Pass the Windows OpenSSH SFTP session display name with -WindowsSession.' }

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
        if (-not $iniData.ContainsKey($Sessions[$index])) { throw "Unix remote session #$($index + 1) is not in sftpplug.ini" }
        $remote += Get-SshSession $Sessions[$index] $iniData[$Sessions[$index]] ($index + 1)
    }
    if (-not $iniData.ContainsKey($WindowsSession)) { throw 'Windows remote session is not in sftpplug.ini' }
    $windowsRemote = Get-SshSession $WindowsSession $iniData[$WindowsSession] 3
    $windowsRemote.Label = 'Windows remote'
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
    $windowsTemp = (Invoke-WindowsRemote $windowsRemote '[Console]::Write($env:TEMP)' | Out-String).Trim()
    if (-not $windowsTemp) { throw 'Windows remote did not return a temporary directory.' }
    $windowsRoot = $windowsTemp.TrimEnd('\') + "\SftpArchiveSmoke-$script:RunId"
    $sftpWindows = "\\SFTP\$WindowsSession\$windowsRoot"
    $tcSftpWindows = "\\\SFTP\$WindowsSession\$windowsRoot"
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

    $largePayload = Join-Path $local 'payload'
    Copy-Item -LiteralPath (Join-Path $local 'fixture-local\.oh-my-zsh') -Destination $largePayload -Recurse
    New-Item -ItemType Directory -Path (Join-Path $largePayload 'nested') -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $largePayload 'nested\value.txt'), 'archive-smoke')
    $largePayloadArchive = Join-Path $local 'payload.tar'
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    $null = & $tar -cf $largePayloadArchive -C $local 'payload' 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'could not create the large archive smoke payload' }

    for ($index = 0; $index -lt $remote.Count; $index++) {
        $session = $remote[$index]
        Write-Host "[INFO] Preparing Unix remote session #$($index + 1) fixture"
        Invoke-Remote $session "rm -rf $(Quote-Remote $r1); mkdir -p $(Quote-Remote "$r1/source") $(Quote-Remote "$r1/f5") $(Quote-Remote "$r1/f6") $(Quote-Remote "$r1/f7") $(Quote-Remote "$r1/f8")"
        Expand-PayloadOnUnixRemote $session $largePayloadArchive "$r1/source"
    }
    $windowsRootLiteral = $windowsRoot.Replace("'", "''")
    Write-Host '[INFO] Preparing Windows remote fixture'
    Invoke-WindowsRemote $windowsRemote "New-Item -ItemType Directory -Path '$windowsRootLiteral\source','$windowsRootLiteral\f5','$windowsRootLiteral\f6','$windowsRootLiteral\f7','$windowsRootLiteral\f8' -Force | Out-Null" | Out-Null
    Expand-PayloadOnWindowsRemote $windowsRemote $largePayloadArchive ($windowsRoot + '\source')

    $tc = Join-Path $TotalCommanderPath 'TOTALCMD64.EXE'
    Activate-RemoteSessions $tc "$tcSftp1\source" 1 "$tcSftp2\source" 2
    Add-Result 'active Unix remote WFX sessions #1 and #2' 'PASS'
    Activate-RemoteSessions $tc "$tcSftpWindows\source" 3 "$tcSftp1\source" 1
    Run-Case 'Alt+F11 Windows remote directory prewarm' {
        Invoke-RouterPrewarm "$sftpWindows\source" 'f11-windows-prewarm'
    }
    Activate-RemoteSessions $tc "$tcSftp1\source" 1 "$tcSftp2\source" 2

    Run-Case 'Alt+F5 local -> Unix remote archive' {
        Invoke-Router pack $local "$sftp1\f5\local.tar" (Join-Path $local 'payload') 'f5-local'
        Invoke-Remote $remote[0] "tar -tf $(Quote-Remote "$r1/f5/local.tar") | grep -q payload/nested/value.txt"
    }
    Run-Case 'Alt+F5 WSL UNC -> Unix remote archive' {
        $source = Join-Path $uncTargets[0] 'f5-source'; New-Item -ItemType Directory -Path $source -Force | Out-Null
        Copy-Item -LiteralPath $largePayload -Destination $source -Recurse
        Invoke-Router pack $source "$sftp1\f5\unc.tar" ((Join-Path $source 'payload') + '\') 'f5-unc'
        Invoke-Remote $remote[0] "tar -tf $(Quote-Remote "$r1/f5/unc.tar") | grep -q payload/nested/value.txt"
    }
    Run-Case 'Alt+F5 Unicode WSL UNC -> Unix remote archive' {
        $source = Join-Path $uncTargets[0] 'unicode-测试-source'; $selected = Join-Path $source 'payload-测试\nested'
        New-Item -ItemType Directory -Path $selected -Force | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $selected 'value.txt'), 'archive-smoke')
        Invoke-Router pack $source "$sftp1\f5\unicode-unc.tar" (Split-Path -Parent $selected) 'f5-unicode-unc'
        Invoke-Remote $remote[0] "tar -tf $(Quote-Remote "$r1/f5/unicode-unc.tar") | grep -q '/value.txt'"
    }
    Run-Case 'Alt+F5 Unix remote -> local archive' {
        Invoke-Router pack "$sftp1\source" (Join-Path $local 'f5-remote.tar') "$sftp1\source\payload" 'f5-remote-local'
        if (-not (& "$env:SystemRoot\System32\tar.exe" -tf (Join-Path $local 'f5-remote.tar') | Select-String -Quiet 'payload/nested/value.txt')) { throw 'local archive missing payload' }
    }
    Run-Case 'Alt+F5 Unix remote -> WSL UNC archive' {
        $target = Join-Path $uncTargets[0] 'f5-remote.tar'
        Invoke-Router pack "$sftp1\source" $target "$sftp1\source\payload" 'f5-remote-unc'
        if (-not (& "$env:SystemRoot\System32\tar.exe" -tf $target | Select-String -Quiet 'payload/nested/value.txt')) { throw 'UNC archive missing payload' }
    }
    Run-Case 'Alt+F5 Unix remote #1 -> Unix remote #2 archive' {
        Invoke-Router pack "$sftp1\source" "$sftp2\f5\cross.tar" "$sftp1\source\payload" 'f5-cross'
        Invoke-Remote $remote[1] "tar -tf $(Quote-Remote "$r2/f5/cross.tar") | grep -q payload/nested/value.txt"
    }
    Activate-RemoteSessions $tc "$tcSftpWindows\source" 3 "$tcSftp1\source" 1
    Run-Case 'Alt+F5 local -> Windows remote archive' {
        $target = "$sftpWindows\f5\local.tar"
        Invoke-Router pack $local $target (Join-Path $local 'payload') 'f5-local-windows'
        Assert-WindowsRemoteTarEntry $windowsRemote "$windowsRoot\f5\local.tar" 'payload/nested/value.txt'
        Assert-WindowsNoArchiveTemps $windowsRemote "$windowsRoot\f5"
    }
    Run-Case 'Alt+F5 failed Windows archive preserves existing target' {
        Assert-RouterFails pack "$sftp1\source" "$sftpWindows\f5\local.tar" "$sftp1\source\missing.tar" 'f5-failed-windows-preserves'
        Assert-WindowsRemoteTarEntry $windowsRemote "$windowsRoot\f5\local.tar" 'payload/nested/value.txt'
        Assert-WindowsNoArchiveTemps $windowsRemote "$windowsRoot\f5"
    }
    Run-Case 'Alt+F5 WSL UNC -> Windows remote archive' {
        $source = Join-Path $uncTargets[0] 'f5-windows-source'; New-Item -ItemType Directory -Path $source -Force | Out-Null
        Copy-Item -LiteralPath $largePayload -Destination $source -Recurse
        Invoke-Router pack $source "$sftpWindows\f5\unc.tar" (Join-Path $source 'payload') 'f5-unc-windows'
        Assert-WindowsRemoteTarEntry $windowsRemote "$windowsRoot\f5\unc.tar" 'payload/nested/value.txt'
    }
    Run-Case 'Alt+F5 Unix remote -> Windows remote archive' {
        Invoke-Router pack "$sftp1\source" "$sftpWindows\f5\unix.tar" "$sftp1\source\payload" 'f5-unix-windows'
        Assert-WindowsRemoteTarEntry $windowsRemote "$windowsRoot\f5\unix.tar" 'payload/nested/value.txt'
    }
    Run-Case 'Alt+F5 Windows remote -> local archive' {
        $target = Join-Path $local 'f5-windows.tar'
        Invoke-Router pack "$sftpWindows\source" $target "$sftpWindows\source\payload" 'f5-windows-local'
        if (-not (& "$env:SystemRoot\System32\tar.exe" -tf $target | Select-String -Quiet 'payload/nested/value.txt')) { throw 'local archive missing Windows payload' }
    }
    Run-Case 'Alt+F5 Windows remote -> Unix remote archive' {
        Invoke-Router pack "$sftpWindows\source" "$sftp1\f5\windows.tar" "$sftpWindows\source\payload" 'f5-windows-unix'
        Invoke-Remote $remote[0] "tar -tf $(Quote-Remote "$r1/f5/windows.tar") | grep -q payload/nested/value.txt"
    }
    Activate-RemoteSessions $tc "$tcSftp1\source" 1 "$tcSftp2\source" 2
    Run-Case 'Alt+F6 local archive -> Unix remote' {
        Invoke-Router unpack $local "$sftp1\f6" (Join-Path $local 'f5-remote.tar') 'f6-local-remote'
        Assert-RemoteFile $remote[0] "$r1/f6/payload/nested/value.txt"
    }
    Run-Case 'Alt+F6 WSL UNC archive -> Unix remote' {
        $archive = Join-Path $uncTargets[0] 'f5-remote.tar'
        Invoke-Router unpack (Split-Path -Parent $archive) "$sftp1\f6" $archive 'f6-unc-remote'
        Assert-RemoteFile $remote[0] "$r1/f6/payload/nested/value.txt"
    }
    Run-Case 'Alt+F6 Unix remote archive -> local' {
        $target = Join-Path $local 'f6-remote-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router unpack "$sftp1\f5" $target 'local.tar' 'f6-remote-local'
        Assert-File (Join-Path $target 'payload\nested\value.txt')
    }
    for ($index = 0; $index -lt $uncTargets.Count; $index++) {
        $uncTarget = $uncTargets[$index]
        Run-Case "Alt+F6 Unix remote archive -> WSL UNC target #$($index + 1)" {
            $target = Join-Path $uncTarget 'f6'; New-Item -ItemType Directory -Path $target -Force | Out-Null
            Invoke-Router unpack "$sftp1\f5" $target 'local.tar' ("f6-remote-unc-{0}" -f ($index + 1))
            Assert-File (Join-Path $target 'payload\nested\value.txt')
        }
    }
    Run-Case 'Alt+F6 Unix remote #1 archive -> Unix remote #2' {
        Invoke-Router unpack "$sftp1\f5" "$sftp2\f6" 'local.tar' 'f6-cross'
        Assert-RemoteFile $remote[1] "$r2/f6/payload/nested/value.txt"
    }
    Activate-RemoteSessions $tc "$tcSftpWindows\source" 3 "$tcSftp1\source" 1
    Run-Case 'Alt+F6 local archive -> Windows remote' {
        Invoke-Router unpack $local "$sftpWindows\f6" (Join-Path $local 'f5-remote.tar') 'f6-local-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f6\payload\nested\value.txt"
    }
    Run-Case 'Alt+F6 WSL UNC archive -> Windows remote' {
        $archive = Join-Path $uncTargets[0] 'f5-remote.tar'
        Invoke-Router unpack (Split-Path -Parent $archive) "$sftpWindows\f6" $archive 'f6-unc-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f6\payload\nested\value.txt"
    }
    Run-Case 'Alt+F6 Unix remote archive -> Windows remote' {
        Invoke-Router unpack "$sftp1\f5" "$sftpWindows\f6" 'local.tar' 'f6-unix-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f6\payload\nested\value.txt"
    }
    Run-Case 'Alt+F6 Windows remote archive -> local' {
        $target = Join-Path $local 'f6-windows-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router unpack "$sftpWindows\f5" $target 'local.tar' 'f6-windows-local'
        Assert-File (Join-Path $target 'payload\nested\value.txt')
    }
    Run-Case 'Alt+F6 Windows remote archive -> Unix remote' {
        Invoke-Router unpack "$sftpWindows\f5" "$sftp1\f6" 'local.tar' 'f6-windows-unix'
        Assert-RemoteFile $remote[0] "$r1/f6/payload/nested/value.txt"
    }
    Activate-RemoteSessions $tc "$tcSftp1\source" 1 "$tcSftp2\source" 2
    Run-Case 'Alt+F7 local -> Unix remote' {
        Invoke-Router copy $local "$sftp1\f7" (Join-Path $local 'payload') 'f7-local-remote'
        Assert-RemoteFile $remote[0] "$r1/f7/payload/nested/value.txt"
    }
    for ($index = 0; $index -lt $uncTargets.Count; $index++) {
        $uncTarget = $uncTargets[$index]
        Run-Case "Alt+F7 WSL UNC target #$($index + 1) -> Unix remote" {
            $source = Join-Path $uncTarget 'f7-source'; New-Item -ItemType Directory -Path $source -Force | Out-Null
            Copy-Item -LiteralPath $largePayload -Destination $source -Recurse
            Invoke-Router copy $source "$sftp1\f7" (Join-Path $source 'payload') ("f7-unc-remote-{0}" -f ($index + 1))
            Assert-RemoteFile $remote[0] "$r1/f7/payload/nested/value.txt"
        }
    }
    Run-Case 'Alt+F7 Unix remote -> local' {
        $target = Join-Path $local 'f7-remote-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router copy "$sftp1\source" $target "$sftp1\source\payload" 'f7-remote-local'
        Assert-File (Join-Path $target 'payload\nested\value.txt')
    }
    for ($index = 0; $index -lt $uncTargets.Count; $index++) {
        $uncTarget = $uncTargets[$index]
        Run-Case "Alt+F7 Unix remote -> WSL UNC target #$($index + 1)" {
            $target = Join-Path $uncTarget 'f7'; New-Item -ItemType Directory -Path $target -Force | Out-Null
            Invoke-Router copy "$sftp1\source" $target "$sftp1\source\payload" ("f7-remote-unc-{0}" -f ($index + 1))
            Assert-File (Join-Path $target 'payload\nested\value.txt')
        }
    }
    Run-Case 'Alt+F7 Unix remote #1 -> Unix remote #2' {
        Invoke-Router copy "$sftp1\source" "$sftp2\f7" "$sftp1\source\payload" 'f7-cross'
        Assert-RemoteFile $remote[1] "$r2/f7/payload/nested/value.txt"
    }
    Activate-RemoteSessions $tc "$tcSftpWindows\source" 3 "$tcSftp1\source" 1
    Run-Case 'Alt+F7 local -> Windows remote' {
        Invoke-Router copy $local "$sftpWindows\f7" (Join-Path $local 'payload') 'f7-local-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f7\payload\nested\value.txt"
    }
    Run-Case 'Alt+F7 WSL UNC -> Windows remote' {
        $source = Join-Path $uncTargets[0] 'f7-windows-source'; New-Item -ItemType Directory -Path $source -Force | Out-Null
        Copy-Item -LiteralPath $largePayload -Destination $source -Recurse
        Invoke-Router copy $source "$sftpWindows\f7" (Join-Path $source 'payload') 'f7-unc-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f7\payload\nested\value.txt"
    }
    Run-Case 'Alt+F7 Unix remote -> Windows remote' {
        Invoke-Router copy "$sftp1\source" "$sftpWindows\f7" "$sftp1\source\payload" 'f7-unix-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f7\payload\nested\value.txt"
    }
    Run-Case 'Alt+F7 Windows remote -> local' {
        $target = Join-Path $local 'f7-windows-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router copy "$sftpWindows\source" $target "$sftpWindows\source\payload" 'f7-windows-local'
        Assert-File (Join-Path $target 'payload\nested\value.txt')
    }
    Run-Case 'Alt+F7 Windows remote -> Unix remote' {
        Invoke-Router copy "$sftpWindows\source" "$sftp1\f7" "$sftpWindows\source\payload" 'f7-windows-unix'
        Assert-RemoteFile $remote[0] "$r1/f7/payload/nested/value.txt"
    }
    Activate-RemoteSessions $tc "$tcSftp1\source" 1 "$tcSftp2\source" 2
    Run-Case 'Alt+F8 local -> Unix remote deletes source after success' {
        $source = Join-Path $local 'f8-local'; New-Item -ItemType Directory -Path $source -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $local 'payload') -Destination $source -Recurse
        Invoke-Router move $source "$sftp1\f8" (Join-Path $source 'payload') 'f8-local-remote'
        Assert-RemoteFile $remote[0] "$r1/f8/payload/nested/value.txt"
        if (Test-Path -LiteralPath (Join-Path $source 'payload')) { throw 'local source was not deleted' }
    }
    Run-Case 'Alt+F8 WSL UNC -> Unix remote deletes source after success' {
        $source = Join-Path $uncTargets[0] 'f8-source'; New-Item -ItemType Directory -Path $source -Force | Out-Null
        Copy-Item -LiteralPath $largePayload -Destination $source -Recurse
        Invoke-Router move $source "$sftp1\f8" (Join-Path $source 'payload') 'f8-unc-remote'
        Assert-RemoteFile $remote[0] "$r1/f8/payload/nested/value.txt"
        if (Test-Path -LiteralPath (Join-Path $source 'payload')) { throw 'UNC source was not deleted' }
    }
    Run-Case 'Alt+F8 Unix remote -> local deletes source after success' {
        $target = Join-Path $local 'f8-remote-local'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router move "$sftp1\source" $target "$sftp1\source\payload" 'f8-remote-local'
        Assert-File (Join-Path $local 'f8-remote-local\payload\nested\value.txt')
        if (Test-RemotePath $remote[0] "$r1/source/payload") { throw 'remote source was not deleted' }
    }
    Run-Case 'Alt+F8 Unix remote -> WSL UNC deletes source after success' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/move-unc")"
        Expand-PayloadOnUnixRemote $remote[0] $largePayloadArchive "$r1/move-unc"
        $target = Join-Path $uncTargets[0] 'f8-remote'; New-Item -ItemType Directory -Path $target -Force | Out-Null
        Invoke-Router move "$sftp1\move-unc" $target "$sftp1\move-unc\payload" 'f8-remote-unc'
        Assert-File (Join-Path $target 'payload\nested\value.txt')
        if (Test-RemotePath $remote[0] "$r1/move-unc/payload") { throw 'remote source was not deleted' }
    }
    Run-Case 'Alt+F8 Unix remote #1 -> Unix remote #2 deletes source after success' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/move")"
        Expand-PayloadOnUnixRemote $remote[0] $largePayloadArchive "$r1/move"
        Invoke-Router move "$sftp1\move" "$sftp2\f8" "$sftp1\move\payload" 'f8-cross'
        Assert-RemoteFile $remote[1] "$r2/f8/payload/nested/value.txt"
        if (Test-RemotePath $remote[0] "$r1/move/payload") { throw 'remote source was not deleted' }
    }
    Run-Case 'Alt+F8 failed Unix remote target preserves source' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/failed/payload/nested"); printf archive-smoke > $(Quote-Remote "$r1/failed/payload/nested/value.txt")"
        Assert-RouterFails move "$sftp1\failed" "$sftp2\missing-parent\target" "$sftp1\failed\payload" 'f8-failed-target'
        Assert-RemoteFile $remote[0] "$r1/failed/payload/nested/value.txt"
    }
    Activate-RemoteSessions $tc "$tcSftpWindows\source" 3 "$tcSftp1\source" 1
    Run-Case 'Alt+F8 local -> Windows remote deletes source after success' {
        $source = Join-Path $local 'f8-windows-local'; New-Item -ItemType Directory -Path $source -Force | Out-Null
        Copy-Item -LiteralPath $largePayload -Destination $source -Recurse
        Invoke-Router move $source "$sftpWindows\f8" (Join-Path $source 'payload') 'f8-local-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f8\payload\nested\value.txt"
        if (Test-Path -LiteralPath (Join-Path $source 'payload')) { throw 'local source was not deleted' }
    }
    Run-Case 'Alt+F8 WSL UNC -> Windows remote deletes source after success' {
        $source = Join-Path $uncTargets[0] 'f8-windows-source'; New-Item -ItemType Directory -Path $source -Force | Out-Null
        Copy-Item -LiteralPath $largePayload -Destination $source -Recurse
        Invoke-Router move $source "$sftpWindows\f8" (Join-Path $source 'payload') 'f8-unc-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f8\payload\nested\value.txt"
        if (Test-Path -LiteralPath (Join-Path $source 'payload')) { throw 'UNC source was not deleted' }
    }
    Run-Case 'Alt+F8 Unix remote -> Windows remote deletes source after success' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/move-windows")"
        Expand-PayloadOnUnixRemote $remote[0] $largePayloadArchive "$r1/move-windows"
        Invoke-Router move "$sftp1\move-windows" "$sftpWindows\f8" "$sftp1\move-windows\payload" 'f8-unix-windows'
        Assert-WindowsRemoteFile $windowsRemote "$windowsRoot\f8\payload\nested\value.txt"
        if (Test-RemotePath $remote[0] "$r1/move-windows/payload") { throw 'Unix remote source was not deleted' }
    }
    Run-Case 'Alt+F8 Windows remote -> local deletes source after success' {
        Invoke-Router move "$sftpWindows\source" (Join-Path $local 'f8-windows-local') "$sftpWindows\source\payload" 'f8-windows-local'
        Assert-File (Join-Path $local 'f8-windows-local\payload\nested\value.txt')
        if (Test-WindowsRemotePath $windowsRemote "$windowsRoot\source\payload") { throw 'Windows remote source was not deleted' }
    }
    Run-Case 'Alt+F8 Windows remote -> Unix remote deletes source after success' {
        Invoke-WindowsRemote $windowsRemote "New-Item -ItemType Directory -Path '$windowsRootLiteral\move-unix' -Force | Out-Null" | Out-Null
        Expand-PayloadOnWindowsRemote $windowsRemote $largePayloadArchive ($windowsRoot + '\move-unix')
        Invoke-Router move "$sftpWindows\move-unix" "$sftp1\f8" "$sftpWindows\move-unix\payload" 'f8-windows-unix'
        Assert-RemoteFile $remote[0] "$r1/f8/payload/nested/value.txt"
        if (Test-WindowsRemotePath $windowsRemote "$windowsRoot\move-unix\payload") { throw 'Windows remote source was not deleted' }
    }
    Run-Case 'Alt+F8 failed Windows target preserves Unix source' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/failed-windows/payload/nested"); printf archive-smoke > $(Quote-Remote "$r1/failed-windows/payload/nested/value.txt")"
        Assert-RouterFails move "$sftp1\failed-windows" "$sftpWindows\missing-parent\target" "$sftp1\failed-windows\payload" 'f8-failed-windows-target'
        Assert-RemoteFile $remote[0] "$r1/failed-windows/payload/nested/value.txt"
    }
    Activate-RemoteSessions $tc "$tcSftp1\source" 1 "$tcSftp2\source" 2
    Run-Case 'Alt+F9 Unix remote deletion' {
        Invoke-Remote $remote[0] "mkdir -p $(Quote-Remote "$r1/delete/tree"); printf archive-smoke > $(Quote-Remote "$r1/delete/tree/value.txt")"
        Invoke-Router delete "$sftp1\delete" '' "$sftp1\delete\tree" 'f9-delete'
        if (Test-RemotePath $remote[0] "$r1/delete/tree") { throw 'remote tree was not deleted' }
    }
    Activate-RemoteSessions $tc "$tcSftpWindows\source" 3 "$tcSftp1\source" 1
    Run-Case 'Alt+F9 Windows remote deletion' {
        Invoke-WindowsRemote $windowsRemote "New-Item -ItemType Directory -Path '$windowsRootLiteral\delete\tree' -Force | Out-Null; Set-Content -LiteralPath '$windowsRootLiteral\delete\tree\value.txt' -Value 'archive-smoke' -NoNewline" | Out-Null
        Invoke-Router delete "$sftpWindows\delete" '' "$sftpWindows\delete\tree" 'f9-windows-delete'
        if (Test-WindowsRemotePath $windowsRemote "$windowsRoot\delete\tree") { throw 'Windows remote tree was not deleted' }
    }
} finally {
    if (Get-Variable tc -Scope Script -ErrorAction SilentlyContinue -ValueOnly) {
        Write-Host '[INFO] Closing test Total Commander instance'
        try { Close-TestTotalCommander $tc } catch { Write-Host '[WARN] Test Total Commander cleanup failed.' }
    }
    if (Get-Variable remote -Scope Script -ErrorAction SilentlyContinue) {
        foreach ($session in $remote) {
            Write-Host "[INFO] Removing $($session.Label) artifacts"
            try { Invoke-Remote $session "rm -rf /tmp/sftpplug-archive-smoke-$script:RunId" } catch { Write-Host "[WARN] $($session.Label) cleanup failed" }
        }
    }
    if (Get-Variable windowsRemote -Scope Script -ErrorAction SilentlyContinue -ValueOnly) {
        Write-Host '[INFO] Removing Windows remote artifacts'
        try {
            $cleanupRoot = $windowsRoot.Replace("'", "''")
            Invoke-WindowsRemote $windowsRemote "Remove-Item -LiteralPath '$cleanupRoot' -Recurse -Force -ErrorAction SilentlyContinue"
        } catch { Write-Host '[WARN] Windows remote cleanup failed' }
    }
    if (-not $KeepArtifacts) {
        Write-Host '[INFO] Removing local artifacts'
        Remove-UncArtifacts $WslDistros $script:RunId
        Remove-Item -LiteralPath $script:WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (@($script:Results | Where-Object Status -eq 'FAIL').Count -gt 0) { exit 1 }
Write-Host 'Archive smoke cleanup complete.' -ForegroundColor Green
exit 0
