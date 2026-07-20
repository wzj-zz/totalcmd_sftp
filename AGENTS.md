# Agent Notes

## Build And Deployment

- Build x64 Release with:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64`
- Output WFX: `build\bin\x64_Release\sftpplug.wfx`.
- A successful build refreshes ignored `SFTP\` with only `SFTPplug.wfx64`, `SftpArchiveRouter.exe`, `SFTPplug.chm`, `sftp.php`, and `language\zh-cn.lng`. Never add these generated artifacts to Git.
- Deploy while Total Commander is stopped: delete the portable `<Total Commander>\Plugins\Wfx\SFTP\` directory, then copy the generated `SFTP\` directory in its place. Do not perform hash verification unless requested. Restart Total Commander afterward.

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
