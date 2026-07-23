# Agent Notes

## Build And Deployment

- Build x64 Release with:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64`
- Output WFX: `build\bin\x64_Release\sftpplug.wfx`.
- A successful build refreshes ignored `SFTP\` with only `SFTPplug.wfx64`, `SftpArchiveRouter.exe`, `SFTPplug.chm`, `sftp.php`, `language\zh-cn.lng`, `7z.exe`, `7z.dll`, and `7zip-License.txt`. Never add these generated artifacts to Git.
- Deploy while Total Commander is stopped: delete the portable `<Total Commander>\Plugins\Wfx\SFTP\` directory, then copy the generated `SFTP\` directory in its place. Run `<Total Commander>\Plugins\Wfx\SFTP\SftpArchiveRouter.exe init -y` to register shortcuts without a success dialog, then restart Total Commander. Use plain `init` only when an interactive success dialog is wanted. Do not perform hash verification unless requested.

## Post-Deploy Archive Smoke Tests

- Standard flow when the user supplies only `<Total Commander>`: run the default preflight below, build Release, deploy the generated `SFTP\` package, then run the default smoke command. Do not rediscover the script parameters or ask for target names when preflight passes.
- Default preflight, replacing `<Total Commander>` before running:
  ```powershell
  $tc = '<Total Commander>'
  $ini = Join-Path $tc 'sftpplug.ini'
  if (-not (Test-Path -LiteralPath (Join-Path $tc 'TOTALCMD64.EXE') -PathType Leaf) -or -not (Test-Path -LiteralPath $ini -PathType Leaf)) { throw 'Total Commander executable or sftpplug.ini is missing.' }
  $iniText = [System.IO.File]::ReadAllText($ini)
  foreach ($name in 'vps', 'mini@', 'win@') { if ($iniText -notmatch ('(?m)^\[' + [regex]::Escape($name) + '\]\s*$')) { throw "Default SFTP session '$name' is missing." } }
  foreach ($distro in 'ubuntu_1', 'ubuntu_2') { & wsl.exe -d $distro -- true; if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path (Join-Path '\\wsl.localhost' $distro) 'tmp') -PathType Container)) { throw "Default WSL target '$distro' is unavailable." } }
  ```
- Release build command:
  ```powershell
  & 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64
  if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
  ```
- Deployment command, replacing `<Total Commander>` before running. It stops Total Commander and archive-router processes, replaces only the portable SFTP plugin directory, registers shortcuts without a dialog, then restarts Total Commander:
  ```powershell
  $tc = '<Total Commander>'
  $target = Join-Path $tc 'Plugins\Wfx\SFTP'
  Get-Process TOTALCMD64, SftpArchiveRouter -ErrorAction SilentlyContinue | Stop-Process -Force
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
- Given only `<Total Commander>`, deploy and run the archive smoke test without asking for target names. Locate the deployment target at `<Total Commander>\Plugins\Wfx\SFTP\` and private session configuration at `<Total Commander>\sftpplug.ini`.
- Unless the user explicitly requests other targets, use Unix SSH/SFTP sessions `vps` and `mini@`, Windows OpenSSH/SFTP session `win@`, and WSL distributions `ubuntu_1` and `ubuntu_2`. Read the corresponding server/user settings only from that INI and use the local SSH agent through `ssh`/`scp`.
- Before deployment or smoke testing, verify that `sftpplug.ini` contains all three default session display names and that both default WSL distributions are available through `wsl.exe` and `\\wsl.localhost\<distro>\tmp`. When all defaults exist, do not ask the user for sessions or WSL targets. When a default is unavailable, report the specific missing prerequisite and stop; do not guess replacements or modify private configuration.
- User-supplied sessions or WSL distributions override the defaults for that run. Local Windows extraction requires no additional input.
- Deploy a completed build by stopping Total Commander and archive-router processes, replacing `<Total Commander>\Plugins\Wfx\SFTP\` with the generated `SFTP\` directory, running `<Total Commander>\Plugins\Wfx\SFTP\SftpArchiveRouter.exe init -y` while Total Commander is stopped, then restarting Total Commander. Automated deployment and smoke flows must use `init -y` so they do not wait for the interactive success dialog. Do not perform hash verification unless requested.
- The archive smoke fixture is tracked at `tests\fixtures\.oh-my-zsh.zip`. Do not replace it with a machine-specific path in tests. Successful archive transfer coverage must use its extracted multi-file payload; reserve tiny fixtures for failure, path, and deletion-semantic assertions that do not exercise transfer volume.
- After deploying a build with default targets, run the scriptable smoke checks with:
  `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\archive-smoke.ps1 -TotalCommanderPath "<Total Commander>" -Sessions vps,mini@ -WindowsSession win@ -WslDistros ubuntu_1,ubuntu_2`
- For an explicit user override, substitute only the user-provided session and WSL target values in that command.
- Do not silently skip the two selected Unix remote sessions, the Windows OpenSSH remote session, or all WSL UNC targets during a requested archive smoke run. Do not print or commit private host, password, key, session, or machine names.
- Smoke output must use generic labels such as `Unix remote session #1`, `Windows remote`, and `WSL UNC target #1`; keep user-specific session names and WSL distro names out of committed files and shared logs.
- The script creates only disposable paths: `%TEMP%\SftpArchiveSmoke-*`, remote `/tmp/sftpplug-archive-smoke-*`, and WSL UNC temp directories under detected `\\wsl.localhost\<distro>\tmp` paths or names supplied with `-WslDistros`. It cleans them up unless `-KeepArtifacts` is supplied.
- The smoke script opens the selected sessions in Total Commander, waits for each active WFX session through `SftpArchiveRouter.exe selftest-session`, then uses `selftest-operation` to execute the same `Pack`, `Unpack`, `Copy`, `Move`, and `Delete` business paths as Alt+F5 through Alt+F9 without target or deletion prompts. SSH is used only to create disposable remote fixtures and assert results; it must not replace the operation under test.
- Smoke coverage: deployed router/runtime presence; local and WSL UNC extraction; Unix and Windows OpenSSH `Alt+F5` archive creation, `Alt+F6` extraction, `Alt+F7` TAR copy, `Alt+F8` move with source deletion only after success and preservation after a forced target failure, and `Alt+F9` tree deletion; plus `Alt+F11` Windows remote prewarm. `Alt+F12` remains manual because its Synchronize Directories UI requires explicit confirmation before remote changes.
- Behaviors that still require UI testing in Total Commander: shortcut registration, target prompt defaults, Cancel/Enter semantics, and Alt+F11/Alt+F12 UI workflows. Passing the smoke script proves the router-to-active-WFX-session business logic, not the GUI interaction layer.

## TAR Router

- `SftpArchiveRouter.exe` must remain beside `SFTPplug.wfx64`; it owns local `tar.exe`/`7z.exe`, while the WFX named-pipe service owns the active SSH session.
- `SftpArchiveRouter.exe init` registers the portable parent Total Commander instance's `Ctrl+G` terminal launch, `Alt+F5` pack, `Alt+F6` unpack, `Alt+F7` TAR copy, `Alt+F8` TAR move, `Alt+F9` remote batch delete, `Alt+F11` directory-tree prewarm, and `Alt+F12` local-mirror directory comparison. `Ctrl+G` opens `cmd.exe` for Windows paths, `wsl.exe -d <distro> --cd <path>` for WSL UNC paths, and `ssh.exe` for SFTP paths with `ServerAliveInterval=30`, `ServerAliveCountMax=120`, and `TCPKeepAlive=yes`. It starts a remote `cmd.exe /K cd /d <path>` for SFTP paths that represent a Windows drive, otherwise an interactive Unix shell in the selected remote directory. It writes `%COMMANDER_PATH%` router paths so the instance remains relocatable. It must run while Total Commander is stopped and must not bind or override native `F5` or `F6`.
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
