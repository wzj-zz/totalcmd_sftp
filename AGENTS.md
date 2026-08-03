# Agent Notes

## Build And Deployment

- Build x64 Release with:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64`
- Output WFX: `build\bin\x64_Release\sftpplug.wfx`.
- A successful build refreshes ignored `SFTP\` with only `SFTPplug.wfx64`, `SftpArchiveRouter.exe`, `SFTPplug.chm`, `sftp.php`, `language\zh-cn.lng`, `7z.exe`, `7z.dll`, and `7zip-License.txt`. Never add these generated artifacts to Git.
- Before any automated verification that starts Total Commander, deployment, or handoff for manual feature verification, first direct both panels of the target Total Commander instance to `C:\`, then close it gracefully. Wait until that instance and its `SftpArchiveRouter` have exited before continuing. This prevents a later startup from restoring an SFTP panel and reconnecting automatically. Do not leave the target Total Commander instance running across test/deployment boundaries; never terminate instances belonging to another Total Commander root.
- Deploy while Total Commander is stopped: delete the portable `<Total Commander>\Plugins\Wfx\SFTP\` directory, then copy the generated `SFTP\` directory in its place. Run `<Total Commander>\Plugins\Wfx\SFTP\SftpArchiveRouter.exe init -y` to register shortcuts without a success dialog, then restart Total Commander. Use plain `init` only when an interactive success dialog is wanted. Do not perform hash verification unless requested.
- **Portable package update** is distinct from instance deployment. When the user says `update TotalCMD64.zip`, `更新 TotalCMD64.zip`, or supplies a ZIP path, run `scripts\update-portable-package.ps1 -ArchivePath '<archive>'`. The script builds the current Release package, temporarily extracts the clean ZIP, replaces only its `Plugins\Wfx\SFTP\` directory, runs `init -y` against the extracted portable root, verifies plugin files plus `Wincmd.ini`/`usercmd.ini`, atomically replaces the original ZIP, and removes its temporary extraction. It must not start the clean instance and must fail if any Total Commander or archive-router process is running.

## Documentation

- Keep `README.md` and `README.zh-CN.md` in sync for user-facing behavior, shortcuts, deployment, and testing instructions. When changing one README, check whether the same user-visible information must be updated in the other before finishing or committing.

## Collaboration

- Before a long-running build, deployment, smoke test, or diagnosis, state what is being run. As soon as it returns, the next action must be a standalone user-visible result message stating whether it passed, failed, or timed out. Do not call any further tool or begin cleanup, retries, investigation, deployment, package update, or commit work until that message has been sent. Do not require the user to interrupt to obtain test status.
- On a test failure, state the failed check and that no deployment, package update, or commit will proceed until it is resolved. For extended investigation, provide concise progress updates rather than working silently.

## Post-Deploy Archive Smoke Tests

- Standard flow when the user supplies only `<Total Commander>`: run the default preflight below, build Release, deploy the generated `SFTP\` package, then run the default smoke command. Do not rediscover the script parameters or ask for target names when preflight passes.
- Total Commander directory is an explicit user input. If a request to build, deploy, or smoke test does not include `<Total Commander>`, ask for that directory before running any preflight, process lookup, instance discovery, build, deployment, or test command. Never infer it from a running `TOTALCMD64` process, scan common installation paths, or select an instance yourself.
- Default preflight, replacing `<Total Commander>` before running:
  ```powershell
  $tc = '<Total Commander>'
  $ini = Join-Path $tc 'sftpplug.ini'
  if (-not (Test-Path -LiteralPath (Join-Path $tc 'TOTALCMD64.EXE') -PathType Leaf) -or -not (Test-Path -LiteralPath $ini -PathType Leaf)) { throw 'Total Commander executable or sftpplug.ini is missing.' }
  $iniText = [System.IO.File]::ReadAllText($ini)
  foreach ($name in 'vps', 'mini@', 'win@') { if ($iniText -notmatch ('(?m)^\[' + [regex]::Escape($name) + '\]\s*$')) { throw "Default SFTP session '$name' is missing." } }
  foreach ($distro in 'ubuntu_1', 'ubuntu_2') { & wsl.exe -d $distro -- true; if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path (Join-Path '\\wsl.localhost' $distro) 'tmp') -PathType Container)) { throw "Default WSL target '$distro' is unavailable." } }
  ```
- The command tool already runs PowerShell. Execute each PowerShell snippet in this document directly as the command text, after replacing only `<Total Commander>`; do not wrap it in `pwsh.exe -Command`, quote it for a child shell, encode it, or create a temporary `.ps1` file. Nested `pwsh.exe -Command "..."` calls expand `$variables` in the outer shell and are forbidden. When an external `pwsh.exe` process is genuinely required, pass the complete script as one literal argument without outer-shell interpolation.
- Release build command:
  ```powershell
  & 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64
  if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
  ```
- Deployment command, replacing `<Total Commander>` before running. It moves both panels to `C:\`, closes Total Commander cleanly, waits for it and the archive router to exit, replaces only the portable SFTP plugin directory, registers shortcuts without a dialog, then restarts Total Commander:
  ```powershell
  $tc = '<Total Commander>'
  $target = Join-Path $tc 'Plugins\Wfx\SFTP'
  $tcExe = [System.IO.Path]::GetFullPath((Join-Path $tc 'TOTALCMD64.EXE'))
  $routerExe = [System.IO.Path]::GetFullPath((Join-Path $target 'SftpArchiveRouter.exe'))
  $tcProcess = @(Get-Process TOTALCMD64 -ErrorAction SilentlyContinue | Where-Object { try { $_.MainModule.FileName -eq $tcExe } catch { $false } })
  $routerProcess = @(Get-Process SftpArchiveRouter -ErrorAction SilentlyContinue | Where-Object { try { $_.MainModule.FileName -eq $routerExe } catch { $false } })
  if ($tcProcess) {
    Start-Process -FilePath (Join-Path $tc 'TOTALCMD64.EXE') -ArgumentList @('/O', '/L=C:\', '/R=C:\') -Wait
    Start-Sleep -Milliseconds 500
    $tcProcess | ForEach-Object { [void]$_.CloseMainWindow() }
    $tcProcess | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
  }
  $routerProcess | ForEach-Object { [void]$_.CloseMainWindow() }
  @($tcProcess + $routerProcess) | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
  $remaining = @($tcProcess + $routerProcess | Where-Object { -not $_.HasExited })
  if ($remaining) { $remaining | Stop-Process -Force; $remaining | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue }
  if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
  Copy-Item -LiteralPath 'SFTP' -Destination $target -Recurse -Force
  $init = Start-Process -FilePath (Join-Path $target 'SftpArchiveRouter.exe') -ArgumentList @('init', '-y') -Wait -PassThru -NoNewWindow
  if ($init.ExitCode -ne 0) { throw "Router init -y failed with exit code $($init.ExitCode)." }
  Start-Process -FilePath (Join-Path $tc 'TOTALCMD64.EXE')
  ```
- Default full smoke command, replacing `<Total Commander>` before running:
  ```powershell
  pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\archive-smoke.ps1 -TotalCommanderPath '<Total Commander>' -Sessions 'vps,mini@' -WindowsSession 'win@' -WslDistros 'ubuntu_1,ubuntu_2'
  ```
- Given only `<Total Commander>`, set both panels to `C:\` and close Total Commander before the default preflight, build Release, deploy, then run both smoke scripts without asking for target names. Locate the deployment target at `<Total Commander>\Plugins\Wfx\SFTP\` and private session configuration at `<Total Commander>\sftpplug.ini`.
- Unless the user explicitly requests other targets, use Unix SSH/SFTP sessions `vps` and `mini@`, Windows OpenSSH/SFTP session `win@`, and WSL distributions `ubuntu_1` and `ubuntu_2`. Read the corresponding server/user settings only from that INI and use the local SSH agent through `ssh`/`scp`.
- Before deployment or smoke testing, verify that `sftpplug.ini` contains all three default session display names and that both default WSL distributions are available through `wsl.exe` and `\\wsl.localhost\<distro>\tmp`. When all defaults exist, do not ask the user for sessions or WSL targets. When a default is unavailable, report the specific missing prerequisite and stop; do not guess replacements, skip that target, modify private configuration, build, or deploy. A user may explicitly provide an alternate complete `-WslDistros` list for that run.
- User-supplied sessions or WSL distributions override the defaults for that run. Local Windows extraction requires no additional input.
- Deploy a completed build by stopping Total Commander and archive-router processes, replacing `<Total Commander>\Plugins\Wfx\SFTP\` with the generated `SFTP\` directory, running `<Total Commander>\Plugins\Wfx\SFTP\SftpArchiveRouter.exe init -y` while Total Commander is stopped, then restarting Total Commander. Automated deployment and smoke flows must use `init -y` so they do not wait for the interactive success dialog. Do not perform hash verification unless requested.
- The archive smoke fixture is tracked at `tests\fixtures\.oh-my-zsh.zip`. Do not replace it with a machine-specific path in tests. Successful archive transfer coverage must use its extracted multi-file payload; reserve tiny fixtures for failure, path, and deletion-semantic assertions that do not exercise transfer volume.
- After deploying a build with default targets, run the scriptable smoke checks with:
  `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\archive-smoke.ps1 -TotalCommanderPath "<Total Commander>" -Sessions vps,mini@ -WindowsSession win@ -WslDistros ubuntu_1,ubuntu_2`
- Run the independent real SSH tunnel smoke test with one selected Unix and Windows OpenSSH profile:
  `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\tunnel-smoke.ps1 -TotalCommanderPath "<Total Commander>" -UnixSession vps -WindowsSession win@`
- Run builds, deployments, and smoke scripts through a long-running process whose stdout and stderr remain observable while it executes. Announce the exact documented command before starting; relay its live generic progress, then send the required standalone pass/fail/timeout result when it exits. Do not capture all output until process exit or use a truncated buffered result as the only test evidence.
- The tunnel smoke script uses active WFX sessions and the router named-pipe operations to verify real `-L`, `-D`, and loopback-only `-R` TCP traffic for both profiles. It temporarily replaces each profile's tunnel rules and restores them in `finally`; do not replace it with direct `ssh -L/-R/-D` tests.
- Tunnel smoke defaults are exactly Unix `vps` and Windows OpenSSH `win@`. Do not infer tunnel targets from the archive smoke `-Sessions` order or automatically select the first Unix or Windows-looking INI profile.
- For a tunnel smoke override, use only the user-supplied `-UnixSession` and `-WindowsSession` values. For an archive smoke override, substitute only the user-supplied `-Sessions`, `-WindowsSession`, and `-WslDistros` values.
- Do not silently skip the two selected Unix remote sessions, the Windows OpenSSH remote session, or all WSL UNC targets during a requested archive smoke run. Do not print or commit private host, password, key, session, or machine names.
- Smoke output must use generic labels such as `Unix remote session #1`, `Windows remote`, and `WSL UNC target #1`; keep user-specific session names, WSL distro names, hosts, users, and raw SSH/router diagnostics out of committed files and shared logs. Store diagnostics only in disposable local artifacts when required.
- The script creates only disposable paths: `%TEMP%\SftpArchiveSmoke-*`, remote `/tmp/sftpplug-archive-smoke-*`, and WSL UNC temp directories under detected `\\wsl.localhost\<distro>\tmp` paths or names supplied with `-WslDistros`. It cleans them up unless `-KeepArtifacts` is supplied.
- The smoke script opens the selected sessions in Total Commander, waits for each active WFX session through `SftpArchiveRouter.exe selftest-session`, then uses `selftest-operation` to execute the same `Pack`, `Unpack`, `Copy`, `Move`, and `Delete` business paths as Alt+F5 through Alt+F9 without target or deletion prompts. SSH is used only to create disposable remote fixtures and assert results; it must not replace the operation under test.
- Smoke coverage: deployed router/runtime presence; local and WSL UNC extraction; Unix and Windows OpenSSH `Alt+F5` archive creation, `Alt+F6` extraction, `Alt+F7` TAR copy, `Alt+F8` move with source deletion only after success and preservation after a forced target failure, and `Alt+F9` tree deletion; plus `Alt+F11` Windows remote prewarm. `Alt+F12` remains manual because its Synchronize Directories UI requires explicit confirmation before remote changes.
- Behaviors that still require UI testing in Total Commander: shortcut registration, target prompt defaults, Cancel/Enter semantics, and Alt+F11/Alt+F12 UI workflows. Passing the smoke script proves the router-to-active-WFX-session business logic, not the GUI interaction layer.

## TAR Router

- `SftpArchiveRouter.exe` must remain beside `SFTPplug.wfx64`; it owns local `tar.exe`/`7z.exe`, while the WFX named-pipe service owns the active SSH session.
- `SftpArchiveRouter.exe init` registers the portable parent Total Commander instance's `Ctrl+G` terminal launch, `Ctrl+Alt+Shift+G` Windows Terminal tab, `Alt+Shift+G` Windows Terminal split, `Alt+F5` pack, `Alt+F6` unpack, `Alt+F7` TAR copy, `Alt+F8` TAR move, `Alt+F9` remote batch delete, `Alt+F11` directory-tree prewarm, and `Alt+F12` local-mirror directory comparison. `Ctrl+G` opens `cmd.exe` for Windows paths, `wsl.exe -d <distro> --cd <path>` for WSL UNC paths, and `ssh.exe` for SFTP paths with `ServerAliveInterval=30`, `ServerAliveCountMax=120`, and `TCPKeepAlive=yes`. It starts a remote `cmd.exe /K cd /d <path>` for SFTP paths that represent a Windows drive, otherwise an interactive Unix shell in the selected remote directory. `Ctrl+Alt+Shift+G` invokes `wt.exe new-tab`, while `Alt+Shift+G` invokes `wt.exe split-pane`; both title the launched shell `local:win`, `wsl:<distro>`, or `ssh:<session>`. The Windows Terminal tab label reflects its focused pane. Both honor the user's Windows Terminal `windowingBehavior`. It writes `%COMMANDER_PATH%` router paths so the instance remains relocatable. It must run while Total Commander is stopped and must not bind or override native `F5` or `F6`.
- The router must prompt for a target before each SFTP TAR operation: archive file path for pack, target directory for unpack/copy/move. Enter keeps the default; Cancel must not start the operation.
- TAR streaming requires active SSH/SFTP and remote `tar`; PHP Agent and LAN Pair are unsupported.
- `Alt+F8` must delete sources only after target success.
- `Alt+F11` prewarms one active SSH/SFTP session's current directory tree into a ten-minute in-memory `WIN32_FIND_DATAW` cache. The cache must be invalidated after a remote mutation or session close and must preserve Total Commander's native sync/patch workflow.
- `Alt+F12` must use isolated `%TEMP%\SftpLocalDiff` mirrors and launch Total Commander's local sync dialog. Only explicitly confirmed changes from an SFTP mirror may be applied remotely; unchanged and successfully applied sessions must be deleted, while failed or declined sessions remain for inspection.

## WFX And Transfers

- Every exported `Fs*` entry point must retain `DllExceptionBarrier` and `dll_invoke` protection.
- Use `FsGetFileW` for remote-to-local, `FsPutFileW` for local-to-remote, and `FsRenMovFileW` for WFX-panel copies.
- Cross-session regular-file copy streams SFTP handles in `PluginEntryPointsFile.cpp`; source deletion for F6 occurs only after success. Cross-session directory copy is unsupported.
- WFX virtual paths use `\\<session name>\\<remote path>`. Never treat local drive or UNC paths as connection names.
- Sessions are thread-scoped. Long-running cross-thread archive work must use `ServerSessionLease` so disconnect waits for it.
- libssh2 is non-blocking: retry `LIBSSH2_ERROR_EAGAIN` after `WaitForSshIo` for opens, reads, and writes.
- Convert remote relative paths with `ToRemotePathA` and preserve Unix paths and hidden names.

## Logging And Scope

- Use `LogProc` / `LogProcW` for Total Commander operation flow. Use `SFTP_LOG` only for internal state and errors.
- Keep `LOG_ENABLED` and `LOG_TO_FILE` set to `0` for releases.
- Do not modify `thirdparty/` unless explicitly requested. Preserve existing worktree changes and never use destructive Git operations.
