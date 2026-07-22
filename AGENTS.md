# Agent Notes

## Build And Deployment

- Build x64 Release with:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64`
- Output WFX: `build\bin\x64_Release\sftpplug.wfx`.
- A successful build refreshes ignored `SFTP\` with only `SFTPplug.wfx64`, `SftpArchiveRouter.exe`, `SFTPplug.chm`, `sftp.php`, `language\zh-cn.lng`, `7z.exe`, `7z.dll`, and `7zip-License.txt`. Never add these generated artifacts to Git.
- Deploy while Total Commander is stopped: delete the portable `<Total Commander>\Plugins\Wfx\SFTP\` directory, then copy the generated `SFTP\` directory in its place. Do not perform hash verification unless requested. Restart Total Commander afterward.

## Post-Deploy Archive Smoke Tests

- Before deployment or post-deploy archive testing, collect these environment-specific inputs from the user: the portable Total Commander directory, the two SSH/SFTP session display names, and the specific name of each WSL distribution to test. Local Windows extraction requires no additional input. Do not ask only for a WSL target count. Do not hard-code these values in docs, scripts, or commits.
- Given `<Total Commander>`, locate the deployment target at `<Total Commander>\Plugins\Wfx\SFTP\` and the private session configuration at `<Total Commander>\sftpplug.ini`. The user need only provide session display names; read the corresponding server/user settings from that INI and use the local SSH agent through `ssh`/`scp`.
- Deploy a completed build by stopping Total Commander and archive-router processes, replacing `<Total Commander>\Plugins\Wfx\SFTP\` with the generated `SFTP\` directory, running `<Total Commander>\Plugins\Wfx\SFTP\SftpArchiveRouter.exe init` while Total Commander is stopped, then restarting Total Commander. Do not perform hash verification unless requested.
- The archive smoke fixture is tracked at `tests\fixtures\.oh-my-zsh.zip`. Do not replace it with a machine-specific path in tests.
- After deploying a build, run the scriptable smoke checks with:
  `pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\archive-smoke.ps1 -TotalCommanderPath "<Total Commander>" -Sessions <session1>,<session2> -WslDistros <distro1>,<distro2>`
- Do not silently skip the two selected remote sessions or all WSL UNC targets during a requested archive smoke run. The script permits `-SkipRemote` and `-SkipUnc` only when the user explicitly requests reduced coverage. Do not print or commit private host, password, key, session, or machine names.
- Smoke output must use generic labels such as `remote session #1` and `WSL UNC target #1`; keep user-specific session names and WSL distro names out of committed files and shared logs.
- The script creates only disposable paths: `%TEMP%\SftpArchiveSmoke-*`, remote `/tmp/sftpplug-archive-smoke-*`, and WSL UNC temp directories under detected `\\wsl.localhost\<distro>\tmp` paths or names supplied with `-WslDistros`. It cleans them up unless `-KeepArtifacts` is supplied.
- The smoke script opens the selected sessions in Total Commander, waits for each active WFX session through `SftpArchiveRouter.exe selftest-session`, then uses `selftest-operation` to execute the same `Pack`, `Unpack`, `Copy`, `Move`, and `Delete` business paths as Alt+F5 through Alt+F9 without target or deletion prompts. SSH is used only to create disposable remote fixtures and assert results; it must not replace the operation under test.
- Smoke coverage: deployed router/runtime presence; local and WSL UNC extraction; Alt+F5 local/UNC-to-remote, remote-to-local/UNC, and remote-to-remote archive creation; Alt+F6 local/UNC archive-to-remote, remote-to-local/UNC, and remote-to-remote extraction; Alt+F7 local/UNC-to-remote, remote-to-local/UNC, and remote-to-remote TAR copy; Alt+F8 local/UNC-to-remote, remote-to-local/UNC, and remote-to-remote move plus source deletion only after success and preservation after a forced target failure; and Alt+F9 remote tree deletion.
- Behaviors that still require UI testing in Total Commander: shortcut registration, target prompt defaults, Cancel/Enter semantics, and Alt+F11/Alt+F12 UI workflows. Passing the smoke script proves the router-to-active-WFX-session business logic, not the GUI interaction layer.

## TAR Router

- `SftpArchiveRouter.exe` must remain beside `SFTPplug.wfx64`; it owns local `tar.exe`/`7z.exe`, while the WFX named-pipe service owns the active SSH session.
- `SftpArchiveRouter.exe init` registers the portable parent Total Commander instance's `Alt+F5` pack, `Alt+F6` unpack, `Alt+F7` TAR copy, `Alt+F8` TAR move, `Alt+F9` remote batch delete, `Alt+F11` directory-tree prewarm, and `Alt+F12` local-mirror directory comparison. It writes `%COMMANDER_PATH%` router paths so the instance remains relocatable. It must run while Total Commander is stopped and must not bind or override native `F5` or `F6`.
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
