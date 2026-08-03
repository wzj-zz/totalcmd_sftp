param(
    [Parameter(Mandatory)] [string]$TotalCommanderPath,
    [Parameter(Mandatory)] [string]$UnixSession,
    [Parameter(Mandatory)] [string]$WindowsSession,
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'

$script:RouterTimeoutMs = 60000
$script:RemoteCommandTimeoutMs = 60000
$script:EchoReadyTimeoutSeconds = 15

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

function Add-Result([string]$Name, [string]$Status, [string]$Details = '') {
    $script:Results.Add([pscustomobject]@{ Name = $Name; Status = $Status; Details = $Details }) | Out-Null
    $line = "[$Status] $Name"
    if ($Details) { $line += " - $Details" }
    Write-Host $line -ForegroundColor $(if ($Status -eq 'PASS') { 'Green' } else { 'Red' })
}

function Run-Case([string]$Name, [scriptblock]$Action) {
    Write-Host "[INFO] Starting $Name"
    try {
        & $Action
        Add-Result $Name 'PASS'
    } catch {
        Add-Result $Name 'FAIL' $_.Exception.Message
    }
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

function Get-SshSession([hashtable]$Section, [string]$Label) {
    if (-not $Section['server'] -or -not $Section['user']) { throw "$Label has no server or user." }
    $hostName = $Section['server']
    $port = 22
    if ($hostName -match '^\[(.+)\]:(\d+)$') { $hostName = $matches[1]; $port = [int]$matches[2] }
    elseif ($hostName -match '^([^:]+):(\d+)$') { $hostName = $matches[1]; $port = [int]$matches[2] }
    elseif ($Section['customport'] -match '^\d+$') { $port = [int]$Section['customport'] }
    return [pscustomobject]@{
        Label = $Label
        Target = "$($Section['user'])@$hostName"
        SshArgs = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20', '-p', [string]$port)
    }
}

function Quote-Remote([string]$Value) {
    return "'" + $Value.Replace("'", "'`"'`"'") + "'"
}

function ConvertTo-PowerShellEncodedCommand([string]$Script) {
    return [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Script))
}

function Get-WindowsRemoteCommand([string]$Script) {
    return 'powershell.exe -NoProfile -NonInteractive -EncodedCommand ' + (ConvertTo-PowerShellEncodedCommand $Script)
}

function Invoke-RemoteCommand($Session, [string]$Command) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = 'ssh.exe'
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Session.SshArgs) { [void]$info.ArgumentList.Add($argument) }
    [void]$info.ArgumentList.Add($Session.Target)
    [void]$info.ArgumentList.Add($Command)
    $process = [System.Diagnostics.Process]::Start($info)
    try {
        $outputTask = $process.StandardOutput.ReadToEndAsync()
        $errorTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($script:RemoteCommandTimeoutMs)) {
            Stop-TestProcess $process
            throw "$($Session.Label): remote command timed out after $($script:RemoteCommandTimeoutMs / 1000) seconds."
        }
        $output = $outputTask.GetAwaiter().GetResult()
        $error = $errorTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw "$($Session.Label): remote command failed ($($process.ExitCode))."
        }
        return @($output -split "`r?`n" | Where-Object { $_ })
    } finally {
        if ($process) { $process.Dispose() }
    }
}

function Start-RemoteCommand($Session, [string]$Command) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = 'ssh.exe'
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Session.SshArgs) { [void]$info.ArgumentList.Add($argument) }
    [void]$info.ArgumentList.Add($Session.Target)
    [void]$info.ArgumentList.Add($Command)
    for ($attempt = 1; $attempt -le 3; ++$attempt) {
        try {
            return [System.Diagnostics.Process]::Start($info)
        } catch {
            $exception = $_.Exception
            while ($exception -and -not ($exception -is [System.ComponentModel.Win32Exception])) {
                $exception = $exception.InnerException
            }
            if (-not $exception -or $exception.NativeErrorCode -ne 5 -or $attempt -eq 3) { throw }
            Start-Sleep -Milliseconds (250 * $attempt)
        }
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

function Invoke-RouterTunnel([string]$Action, [string]$Path, [string]$Argument, [string]$ResultName) {
    $resultPath = Join-Path $script:WorkRoot "$ResultName.result"
    $errorPath = Join-Path $script:WorkRoot "$ResultName.err"
    Remove-Item -LiteralPath $resultPath, $errorPath -Force -ErrorAction SilentlyContinue
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $script:Router
    $info.UseShellExecute = $false
    $info.RedirectStandardError = $true
    foreach ($value in @('selftest-tunnel', $Action, $Path, $Argument, $resultPath, $errorPath)) { [void]$info.ArgumentList.Add($value) }
    $process = [System.Diagnostics.Process]::Start($info)
    $standardErrorTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($script:RouterTimeoutMs)) {
        Stop-TestProcess $process
        throw "router tunnel $Action timed out after $($script:RouterTimeoutMs / 1000) seconds."
    }
    $standardError = $standardErrorTask.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) {
        $error = if (Test-Path -LiteralPath $errorPath) { Get-Content -LiteralPath $errorPath -Raw } else { $standardError }
        throw "router tunnel $Action failed."
    }
    if ($Action -eq 'status') { return [System.IO.File]::ReadAllText($resultPath) }
    return ''
}

function Wait-ForSession([string]$Path, [string]$Label) {
    $errorPath = Join-Path $script:WorkRoot "$Label-session.err"
    $standardError = ''
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    do {
        Remove-Item -LiteralPath $errorPath -Force -ErrorAction SilentlyContinue
        $info = [System.Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $script:Router
        $info.UseShellExecute = $false
        $info.RedirectStandardError = $true
        foreach ($argument in @('selftest-session', $Path, $errorPath)) { [void]$info.ArgumentList.Add($argument) }
        $process = [System.Diagnostics.Process]::Start($info)
        $standardErrorTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($script:RouterTimeoutMs)) {
            Stop-TestProcess $process
            throw "$Label session probe timed out after $($script:RouterTimeoutMs / 1000) seconds."
        }
        $standardError = $standardErrorTask.GetAwaiter().GetResult()
        if ($process.ExitCode -eq 0) { return }
        Start-Sleep -Milliseconds 750
    } while ([DateTime]::UtcNow -lt $deadline)
    $error = if (Test-Path -LiteralPath $errorPath) { Get-Content -LiteralPath $errorPath -Raw } else { '' }
    if (-not $error) { $error = $standardError }
    throw "$Label did not become active."
}

function Get-AvailableLoopbackPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        return ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Get-AvailableRemoteLoopbackPort($Session, [string]$Kind) {
    if ($Kind -eq 'Unix') {
        $python = "import socket; s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()"
        $output = Invoke-RemoteCommand $Session ("python3 -c " + (Quote-Remote $python))
    } else {
        $script = "`$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0); `$listener.Start(); try { [Console]::Write((([Net.IPEndPoint]`$listener.LocalEndpoint).Port)) } finally { `$listener.Stop() }"
        $output = Invoke-RemoteCommand $Session (Get-WindowsRemoteCommand $script)
    }
    $portText = ($output | Out-String).Trim()
    $port = 0
    if (-not [int]::TryParse($portText, [ref]$port) -or $port -le 0 -or $port -gt 65535) {
        throw 'Remote host did not provide a valid loopback port.'
    }
    return $port
}

function Read-Exact([System.IO.Stream]$Stream, [int]$Length) {
    $bytes = [byte[]]::new($Length)
    $offset = 0
    while ($offset -lt $Length) {
        $read = $Stream.Read($bytes, $offset, $Length - $offset)
        if ($read -le 0) { throw 'TCP peer closed before its response was complete.' }
        $offset += $read
    }
    return $bytes
}

function Invoke-TcpEcho([int]$Port, [int]$TargetPort, [switch]$Socks) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync([System.Net.IPAddress]::Loopback, $Port)
        if (-not $connect.Wait(5000)) { throw 'TCP connection timed out.' }
        $null = $connect.GetAwaiter().GetResult()
        $stream = $client.GetStream()
        $stream.ReadTimeout = 5000
        $stream.WriteTimeout = 5000
        if ($Socks) {
            $stream.Write([byte[]](5, 1, 0), 0, 3)
            $greeting = Read-Exact $stream 2
            if ($greeting[0] -ne 5 -or $greeting[1] -ne 0) { throw 'SOCKS5 server rejected no-authentication mode.' }
            $request = [byte[]](5, 1, 0, 1, 127, 0, 0, 1, ($TargetPort -shr 8), ($TargetPort -band 0xff))
            $stream.Write($request, 0, $request.Length)
            $reply = Read-Exact $stream 10
            if ($reply[0] -ne 5 -or $reply[1] -ne 0) { throw "SOCKS5 CONNECT failed with code $($reply[1])." }
        }
        $payload = [Text.Encoding]::ASCII.GetBytes('tunnel-smoke')
        $stream.Write($payload, 0, $payload.Length)
        $expected = [Text.Encoding]::ASCII.GetBytes('TUNNEL:tunnel-smoke')
        $actual = Read-Exact $stream $expected.Length
        if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$actual, [byte[]]$expected)) { throw 'Tunnel returned an unexpected echo response.' }
    } finally {
        $client.Dispose()
    }
}

function Test-TcpPortClosed([int]$Port) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync([System.Net.IPAddress]::Loopback, $Port)
        if (-not $connect.Wait(1000)) { return $true }
        try { $connect.GetAwaiter().GetResult(); return $false } catch { return $true }
    } finally {
        $client.Dispose()
    }
}

function Start-RemoteEchoServer($Session, [string]$Kind, [int]$Port) {
    if ($Kind -eq 'Unix') {
        $python = "import socket; s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind(('127.0.0.1',$Port)); s.listen(1); print('READY',flush=True); c,_=s.accept(); d=c.recv(64); c.sendall(b'TUNNEL:'+d); c.close(); s.close()"
        return Start-RemoteCommand $Session ("python3 -c " + (Quote-Remote $python))
    }
    $script = "`$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$Port); `$listener.Start(); [Console]::WriteLine('READY'); try { `$client=`$listener.AcceptTcpClient(); try { `$stream=`$client.GetStream(); `$buffer=[byte[]]::new(64); `$count=`$stream.Read(`$buffer,0,`$buffer.Length); `$reply=[Text.Encoding]::ASCII.GetBytes('TUNNEL:' + [Text.Encoding]::ASCII.GetString(`$buffer,0,`$count)); `$stream.Write(`$reply,0,`$reply.Length) } finally { `$client.Dispose() } } finally { `$listener.Stop() }"
    return Start-RemoteCommand $Session (Get-WindowsRemoteCommand $script)
}

function Wait-ForRemoteEchoServer([System.Diagnostics.Process]$Process) {
    $readTask = $Process.StandardOutput.ReadLineAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds($script:EchoReadyTimeoutSeconds)
    do {
        if ($Process.HasExited) { throw 'Remote echo server exited before becoming ready.' }
        if ($readTask.IsCompleted) {
            $line = $readTask.GetAwaiter().GetResult()
            if ($line -eq 'READY') { return }
            throw "Remote echo server returned an invalid ready response: $line"
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Remote echo server did not become ready within $script:EchoReadyTimeoutSeconds seconds."
}

function Start-RemoteEchoClient($Session, [string]$Kind, [int]$Port) {
    if ($Kind -eq 'Unix') {
        $python = "import socket,time; c=None; deadline=time.time()+20;`nwhile time.time()<deadline:`n try: c=socket.create_connection(('127.0.0.1',$Port),1); break`n except OSError: time.sleep(.25)`nif c is None: raise SystemExit(1)`nc.settimeout(10); c.sendall(b'tunnel-smoke'); print(c.recv(64).decode()); c.close()"
        return Start-RemoteCommand $Session ("python3 -c " + (Quote-Remote $python))
    }
    $script = "`$deadline=[DateTime]::UtcNow.AddSeconds(20); `$client=`$null; while ([DateTime]::UtcNow -lt `$deadline) { try { `$client=[Net.Sockets.TcpClient]::new('127.0.0.1',$Port); break } catch { if (`$client) { `$client.Dispose() }; `$client=`$null; Start-Sleep -Milliseconds 250 } }; if (-not `$client) { exit 1 }; try { `$stream=`$client.GetStream(); `$payload=[Text.Encoding]::ASCII.GetBytes('tunnel-smoke'); `$stream.Write(`$payload,0,`$payload.Length); `$buffer=[byte[]]::new(64); `$count=`$stream.Read(`$buffer,0,`$buffer.Length); [Console]::Write([Text.Encoding]::ASCII.GetString(`$buffer,0,`$count)) } finally { `$client.Dispose() }"
    return Start-RemoteCommand $Session (Get-WindowsRemoteCommand $script)
}

function Wait-RemoteEchoClient([System.Diagnostics.Process]$Process) {
    if (-not $Process.WaitForExit(30000)) {
        Stop-TestProcess $Process
        throw 'Remote reverse-tunnel client timed out.'
    }
    $output = $Process.StandardOutput.ReadToEnd().Trim()
    $null = $Process.StandardError.ReadToEnd()
    if ($Process.ExitCode -ne 0) { throw "Remote reverse-tunnel client failed ($($Process.ExitCode))." }
    if ($output -ne 'TUNNEL:tunnel-smoke') { throw 'Remote reverse-tunnel client did not receive the echo response.' }
}

function Start-LocalEchoServer([int]$Port) {
    $script = "`$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$Port); `$listener.Start(); [Console]::WriteLine('READY'); try { `$client=`$listener.AcceptTcpClient(); try { `$stream=`$client.GetStream(); `$buffer=[byte[]]::new(64); `$count=`$stream.Read(`$buffer,0,`$buffer.Length); `$reply=[Text.Encoding]::ASCII.GetBytes('TUNNEL:' + [Text.Encoding]::ASCII.GetString(`$buffer,0,`$count)); `$stream.Write(`$reply,0,`$reply.Length) } finally { `$client.Dispose() } } finally { `$listener.Stop() }"
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = (Get-Process -Id $PID).Path
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    [void]$info.ArgumentList.Add('-NoProfile')
    [void]$info.ArgumentList.Add('-NonInteractive')
    [void]$info.ArgumentList.Add('-EncodedCommand')
    [void]$info.ArgumentList.Add((ConvertTo-PowerShellEncodedCommand $script))
    return [System.Diagnostics.Process]::Start($info)
}

function Write-TunnelRules([string[]]$Rules, [string]$Name) {
    $path = Join-Path $script:WorkRoot "$Name.rules"
    $text = $Rules -join "`n"
    if ($Rules.Count) { $text += "`n" }
    [System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
    return $path
}

function Get-TunnelRules([string]$Status) {
    $rules = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $Status -split "`r?`n") {
        if ($line.Length -gt 2 -and ($line[0] -eq '0' -or $line[0] -eq '1') -and $line[1] -eq "`t") { [void]$rules.Add($line.Substring(2)) }
    }
    return $rules.ToArray()
}

function Assert-TunnelStatus([string]$Path, [bool]$Enabled, [string]$Name) {
    $status = Invoke-RouterTunnel status $Path '-' "$Name-status"
    $expected = if ($Enabled) { '1' } else { '0' }
    if ($status -notmatch "(?m)^$expected`t") { throw "Tunnel status did not report $($Enabled ? 'Enabled' : 'Disabled')." }
}

function Set-TunnelRules([string]$Path, [string[]]$Rules, [string]$Name) {
    $rulesPath = Write-TunnelRules $Rules "$Name-replace"
    Invoke-RouterTunnel replace $Path $rulesPath "$Name-replace" | Out-Null
}

function Assert-TunnelRulesRestored($Profile) {
    $actualRules = @(
        Get-TunnelRules (Invoke-RouterTunnel status $Profile.SftpPath '-' "$($Profile.Label)-restored")
    )
    if ($actualRules.Count -ne $Profile.OriginalRules.Count) {
        throw 'Tunnel rule count did not match the profile snapshot after cleanup.'
    }
    for ($index = 0; $index -lt $actualRules.Count; ++$index) {
        if ($actualRules[$index] -ne $Profile.OriginalRules[$index]) {
            throw 'Tunnel rule content did not match the profile snapshot after cleanup.'
        }
    }
}

function Test-TunnelProfile($Profile) {
    $path = $Profile.SftpPath
    Write-Host "[INFO] Snapshotting $($Profile.Label) tunnel rules"
    $Profile.OriginalRules = @(
        Get-TunnelRules (Invoke-RouterTunnel status $path '-' "$($Profile.Label)-initial")
    )
    $Profile.RulesSnapshotTaken = $true

    try {
        Run-Case "$($Profile.Label) local forwarding (-L)" {
        $remotePort = Get-AvailableRemoteLoopbackPort $Profile.Ssh $Profile.Kind
        $localPort = Get-AvailableLoopbackPort
        $remoteServer = Start-RemoteEchoServer $Profile.Ssh $Profile.Kind $remotePort
        try {
            Wait-ForRemoteEchoServer $remoteServer
            Set-TunnelRules $path @("- -L 127.0.0.1:${localPort}:127.0.0.1:${remotePort}") "$($Profile.Label)-local"
            Invoke-RouterTunnel toggle $path '0|1' "$($Profile.Label)-local-enable" | Out-Null
            Assert-TunnelStatus $path $true "$($Profile.Label)-local-enabled"
            Invoke-TcpEcho $localPort $remotePort
            Invoke-RouterTunnel toggle $path '0|0' "$($Profile.Label)-local-disable" | Out-Null
            Assert-TunnelStatus $path $false "$($Profile.Label)-local-disabled"
            if (-not (Test-TcpPortClosed $localPort)) { throw 'Local forwarding listener remained reachable after disabling it.' }
        } finally {
            Stop-TestProcess $remoteServer
        }
        }

        Run-Case "$($Profile.Label) dynamic SOCKS5 forwarding (-D)" {
        $remotePort = Get-AvailableRemoteLoopbackPort $Profile.Ssh $Profile.Kind
        $localPort = Get-AvailableLoopbackPort
        $remoteServer = Start-RemoteEchoServer $Profile.Ssh $Profile.Kind $remotePort
        try {
            Wait-ForRemoteEchoServer $remoteServer
            Set-TunnelRules $path @("- -D 127.0.0.1:${localPort}") "$($Profile.Label)-dynamic"
            Invoke-RouterTunnel toggle $path '0|1' "$($Profile.Label)-dynamic-enable" | Out-Null
            Assert-TunnelStatus $path $true "$($Profile.Label)-dynamic-enabled"
            Invoke-TcpEcho $localPort $remotePort -Socks
            Invoke-RouterTunnel toggle $path '0|0' "$($Profile.Label)-dynamic-disable" | Out-Null
            Assert-TunnelStatus $path $false "$($Profile.Label)-dynamic-disabled"
            if (-not (Test-TcpPortClosed $localPort)) { throw 'Dynamic SOCKS5 listener remained reachable after disabling it.' }
        } finally {
            Stop-TestProcess $remoteServer
        }
        }

        Run-Case "$($Profile.Label) remote forwarding (-R)" {
        $localPort = Get-AvailableLoopbackPort
        $remotePort = Get-AvailableRemoteLoopbackPort $Profile.Ssh $Profile.Kind
        $localServer = Start-LocalEchoServer $localPort
        $remoteClient = Start-RemoteEchoClient $Profile.Ssh $Profile.Kind $remotePort
        try {
            Wait-ForRemoteEchoServer $localServer
            Set-TunnelRules $path @("- -R 127.0.0.1:${remotePort}:127.0.0.1:${localPort}") "$($Profile.Label)-remote"
            Invoke-RouterTunnel toggle $path '0|1' "$($Profile.Label)-remote-enable" | Out-Null
            Assert-TunnelStatus $path $true "$($Profile.Label)-remote-enabled"
            Wait-RemoteEchoClient $remoteClient
            if (-not $localServer.WaitForExit(5000) -or $localServer.ExitCode -ne 0) { throw 'Local reverse-tunnel echo server did not complete successfully.' }
            Invoke-RouterTunnel toggle $path '0|0' "$($Profile.Label)-remote-disable" | Out-Null
            Assert-TunnelStatus $path $false "$($Profile.Label)-remote-disabled"
        } finally {
            Stop-TestProcess $localServer
            Stop-TestProcess $remoteClient
        }
        }
    } finally {
        if ($Profile.RulesSnapshotTaken) {
            try {
                Write-Host "[INFO] Restoring $($Profile.Label) tunnel rules"
                Set-TunnelRules $path $Profile.OriginalRules "$($Profile.Label)-restore"
                Assert-TunnelRulesRestored $Profile
            } catch {
                Add-Result "$($Profile.Label) tunnel rule restoration" 'FAIL' $_.Exception.Message
            }
        }
    }
}

$TotalCommanderPath = [System.IO.Path]::GetFullPath($TotalCommanderPath)
$script:Results = [System.Collections.Generic.List[object]]::new()
$script:RunId = [Guid]::NewGuid().ToString('N').Substring(0, 12)
$script:WorkRoot = Join-Path ([System.IO.Path]::GetTempPath()) "SftpTunnelSmoke-$script:RunId"
$plugin = Join-Path $TotalCommanderPath 'Plugins\Wfx\SFTP'
$script:Router = Join-Path $plugin 'SftpArchiveRouter.exe'
$ini = Join-Path $TotalCommanderPath 'sftpplug.ini'

try {
    foreach ($path in @($script:Router, (Join-Path $plugin 'SFTPplug.wfx64'), $ini, (Join-Path $TotalCommanderPath 'TOTALCMD64.EXE'))) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw 'required deployed runtime or configuration is missing' }
    }
    $iniData = Read-IniFile $ini
    if (-not $iniData.ContainsKey($UnixSession)) { throw 'Unix remote session is not in sftpplug.ini' }
    if (-not $iniData.ContainsKey($WindowsSession)) { throw 'Windows remote session is not in sftpplug.ini' }
    New-Item -ItemType Directory -Path $script:WorkRoot -Force | Out-Null

    $unix = [pscustomobject]@{ Label = 'Unix remote'; Kind = 'Unix'; Ssh = Get-SshSession $iniData[$UnixSession] 'Unix remote'; SftpPath = $null; TcPath = $null; OriginalRules = @(); RulesSnapshotTaken = $false }
    $windowsTemp = (Invoke-RemoteCommand (Get-SshSession $iniData[$WindowsSession] 'Windows remote') (Get-WindowsRemoteCommand '[Console]::Write($env:TEMP)') | Out-String).Trim()
    if (-not $windowsTemp) { throw 'Windows remote did not return a temporary directory.' }
    $windows = [pscustomobject]@{ Label = 'Windows remote'; Kind = 'Windows'; Ssh = Get-SshSession $iniData[$WindowsSession] 'Windows remote'; SftpPath = $null; TcPath = $null; OriginalRules = @(); RulesSnapshotTaken = $false }
    Invoke-RemoteCommand $unix.Ssh 'python3 --version' | Out-Null
    Invoke-RemoteCommand $windows.Ssh (Get-WindowsRemoteCommand '$PSVersionTable.PSVersion.ToString()') | Out-Null

    $unixRoot = "/tmp/sftpplug-tunnel-smoke-$script:RunId"
    Invoke-RemoteCommand $unix.Ssh ("mkdir -p " + (Quote-Remote $unixRoot)) | Out-Null
    $windowsRoot = $windowsTemp.TrimEnd('\') + "\SftpTunnelSmoke-$script:RunId"
    $windowsRootLiteral = $windowsRoot.Replace("'", "''")
    Invoke-RemoteCommand $windows.Ssh (Get-WindowsRemoteCommand "New-Item -ItemType Directory -Path '$windowsRootLiteral' -Force | Out-Null") | Out-Null
    $unix.SftpPath = "\\SFTP\$UnixSession$($unixRoot.Replace('/', '\'))"
    $unix.TcPath = "\\\SFTP\$UnixSession$($unixRoot.Replace('/', '\'))"
    $windows.SftpPath = "\\SFTP\$WindowsSession\$windowsRoot"
    $windows.TcPath = "\\\SFTP\$WindowsSession\$windowsRoot"

    $tc = Join-Path $TotalCommanderPath 'TOTALCMD64.EXE'
    Start-TotalCommanderSessions $tc $unix.TcPath $windows.TcPath
    Wait-ForSession $unix.SftpPath 'Unix remote'
    Wait-ForSession $windows.SftpPath 'Windows remote'
    Add-Result 'active Unix and Windows remote WFX sessions' 'PASS'

    Test-TunnelProfile $unix
    Test-TunnelProfile $windows
} finally {
    if (Get-Variable tc -Scope Script -ErrorAction SilentlyContinue -ValueOnly) {
        Write-Host '[INFO] Closing test Total Commander instance'
        try { Close-TestTotalCommander $tc } catch { Write-Host '[WARN] Test Total Commander cleanup failed.' }
    }
    if ((Get-Variable unix -Scope Script -ErrorAction SilentlyContinue -ValueOnly) -and $unixRoot) {
        Write-Host '[INFO] Removing Unix remote artifacts'
        try { Invoke-RemoteCommand $unix.Ssh ("rm -rf " + (Quote-Remote $unixRoot)) | Out-Null } catch { Write-Host '[WARN] Unix remote cleanup failed' }
    }
    if ((Get-Variable windows -Scope Script -ErrorAction SilentlyContinue -ValueOnly) -and $windowsRoot) {
        Write-Host '[INFO] Removing Windows remote artifacts'
        try { Invoke-RemoteCommand $windows.Ssh (Get-WindowsRemoteCommand "Remove-Item -LiteralPath '$windowsRootLiteral' -Recurse -Force -ErrorAction SilentlyContinue") | Out-Null } catch { Write-Host '[WARN] Windows remote cleanup failed' }
    }
    if (-not $KeepArtifacts) {
        Write-Host '[INFO] Removing local artifacts'
        Remove-Item -LiteralPath $script:WorkRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if (@($script:Results | Where-Object Status -eq 'FAIL').Count -gt 0) { exit 1 }
Write-Host 'Tunnel smoke cleanup complete.' -ForegroundColor Green
exit 0
