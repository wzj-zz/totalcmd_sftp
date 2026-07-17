# Agent Notes

## Build

- Build the x64 release plugin with:
  `& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64`
- The output plugin is `build\bin\x64_Release\sftpplug.wfx`.
- Do not assume an edited DLL is loaded by Total Commander. Total Commander must be fully restarted after replacing the configured WFX file.

## Total Commander WFX Callbacks

- Every exported `Fs*` entry point must retain `DllExceptionBarrier` and `dll_invoke` protection.
- Remote-to-local downloads use `FsGetFileW`; local-to-remote uploads use `FsPutFileW`.
- Copying between two WFX panels uses `FsRenMovFileW` with `FS_STATUS_OP_RENMOV_SINGLE` or `FS_STATUS_OP_RENMOV_MULTI`, not `FsGetFileW`.
- Cross-session regular-file copy is implemented in `PluginEntryPointsFile.cpp`. It streams source SFTP data to the target SFTP handle in memory. F6 deletes the source only after a successful copy.
- Cross-session directory copy is intentionally unsupported. Do not silently treat a WFX virtual path as a Windows path.
- A WFX virtual path has the form `\\<session name>\\<remote path>`. Local paths such as `C:\\...`, `Z:\\...`, and UNC paths must never be interpreted as connection names.

## Connection Lookup

- Sessions are normally thread-scoped in `ServerRegistry` because TC uses worker threads for transfers.
- `GetServerIdFromAnyThread` is required for cross-panel operations: it resolves an active session by display name regardless of worker thread.
- If an active session cannot be found, cross-session transfers may establish a temporary connection from the saved profile. Temporary connections must be closed with `SftpCloseConnection`, `StopSshKeepAlive`, then `delete`.

## SFTP Transfers

- libssh2 operates non-blocking. File open, read, and write paths must handle `LIBSSH2_ERROR_EAGAIN` by waiting with `WaitForSshIo` and retrying. A single failed `open` is not a sufficient failure condition.
- Convert remote relative paths using `ToRemotePathA` and preserve Unix paths and hidden filenames such as `/.zsh_history`.

## Logging

- Total Commander operation logs use `LogProc` / `LogProcW` and appear in TC's `wcftplog.txt`. Use them for user-facing operation flow.
- Development diagnostics use `SFTP_LOG(tag, fmt, ...)` from `global.h`. Tags are subsystem names such as `CONN`, `AUTH`, `FIND`, `PHP`, `LAN`, and `REMOTE_COPY`.
- Keep `LOG_ENABLED` and `LOG_TO_FILE` set to `0` for normal release builds. When explicitly diagnosing an issue, enable them temporarily to write `C:\temp\sftpplug.log`, then disable them again before delivering the release build.
- Do not duplicate routine operation logs in both systems. Reserve `SFTP_LOG` for internal state, protocol details, retries, and error codes.

## Scope

- Do not modify third-party code under `thirdparty/` unless the task explicitly requires it.
- Preserve existing user/worktree changes. Do not use destructive git operations.
