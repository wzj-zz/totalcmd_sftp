# Secure FTP Plugin for Total Commander

Modern C++20 x64 filesystem plugin for Total Commander. It supports SSH/SFTP, SCP, shell fallback, PHP Agent, and LAN Pair connections.

## Features

- SFTP transfers with resume support and non-blocking libssh2 I/O.
- SCP and shell fallback (`cat`/`dd`/`base64`) for restricted SSH servers.
- ProxyJump through a saved session, HTTP CONNECT, SOCKS4/4a/5, IPv4, and IPv6.
- Password, keyboard-interactive, SSH agent, PEM/OpenSSH, and native PPK v2/v3 authentication.
- DPAPI and Total Commander master-password integration.
- PHP Agent and PHP Shell through the supplied `sftp.php`.
- LAN Pair direct Windows-to-Windows transfers.
- PuTTY, WinSCP, and KiTTY session import.
- Simplified Chinese external translation; English compiled fallback.

## Architecture

```text
Total Commander WFX API
  -> Plugin entry points and exception barrier
  -> Connection, authentication, and session registry
  -> SFTP, SCP, shell, PHP Agent, or LAN Pair transfer backend
  -> libssh2 / WinHTTP / Windows networking APIs
```

`DllExceptionBarrier` protects every exported `Fs*` function so C++ exceptions do not cross the Total Commander ABI.

`ServerRegistry` owns active sessions. Cross-session file copies stream data between active SFTP handles. The archive pipe uses a session lease so an active SSH connection cannot be closed while a router operation is using it.

## SFTP TAR Router

`SftpArchiveRouter.exe` is required beside `SFTPplug.wfx64`. The router owns local `tar.exe` and `7z.exe` work; the plugin owns the active SSH session and remote TAR command through a named pipe. For a portable Total Commander installation, run `SftpArchiveRouter.exe init` once from the deployed plugin directory with Total Commander closed to register the archive commands. It writes `%COMMANDER_PATH%` command paths, so the whole portable directory can later move without reinitialization.

| Shortcut | Operation |
| --- | --- |
| `Alt+F5` | Create a retained `.tar` archive. |
| `Alt+F6` | Extract one selected archive. |
| `Alt+F7` | Accelerated TAR copy when either panel is SFTP. |
| `Alt+F8` | Accelerated TAR move when either panel is SFTP. Sources are removed only after target success. |
| `Alt+F9` | Batch-delete selected SFTP files and directories with one remote `rm -rf` command. |
| `Alt+F11` | Prewarm the current SFTP directory tree for faster native sync directory comparison. |
| `Alt+F12` | Mirror SFTP panel directories to temporary local folders and open TC's local Synchronize Directories view. |
| `Ctrl+P` | Manage SSH Local (`-L`), Remote (`-R`), and Dynamic SOCKS5 (`-D`) tunnels for the active SFTP session. |

The router shows a target input before each SFTP TAR operation. Pack operations default to the generated `.tar` filename; copy, move, and unpack default to the target directory. Press Enter to use the default or edit the target before continuing. Native `F5` and `F6` remain unchanged. TAR streaming requires an active SSH/SFTP session and remote `tar`; PHP Agent and LAN Pair connections are unsupported. `Alt+F6` requires `7z.exe` in `PATH` or `C:\Program Files\7-Zip`.

## SSH Tunnels

Use **Tunnels...** in a connection's settings to enter one rule per line. `+` starts that rule after the SSH/SFTP session connects; `-` stores it disabled. `Ctrl+P` opens a per-session manager that can add, edit, remove, enable, and disable rules without reconnecting. Enable/disable changes are saved back to the session and reused on the next connect.

The `Ctrl+P` **Add...** dialog has built-in Local, Remote, and Dynamic SOCKS5 templates:

```text
- -L 0.0.0.0:2260:127.0.0.1:2260
- -R 0.0.0.0:1080:127.0.0.1:1080
- -D 0.0.0.0:1081
```

Profiles do not receive tunnel rules automatically. Add only the templates you need, then toggle them on or off for the current and future connections.

```text
+ -L 127.0.0.1:8080:app.internal:80
- -R 0.0.0.0:2222:127.0.0.1:22
+ -D [::1]:1080
```

`-L` and `-R` use `[bind_address:]listen_port:target_host:target_port`; `-D` uses `[bind_address:]listen_port`. Bracket IPv6 addresses, for example `[::1]`. Remote listeners may use any address allowed by the SSH server. SSH keepalive is equivalent to `ServerAliveInterval=30`, `ServerAliveCountMax=120`, and `TCPKeepAlive=yes`.

`Alt+F11` runs one remote `find` command for the current SFTP directory tree and holds its names, sizes, and modification times in memory for ten minutes. Total Commander still performs its normal sync directory compare, patch selection, and copy operations through the WFX API. The cache is dropped after a successful remote write, rename, delete, move, or disconnect.

`Alt+F12` is independent from the SFTP metadata cache. It supports SFTP-to-local and SFTP-to-SFTP comparisons by downloading each SFTP panel directory as a TAR stream into `%TEMP%\SftpLocalDiff\<session>`, then opening Total Commander's local Synchronize Directories window. When that window closes, changed SFTP mirrors are summarized and require explicit confirmation before their additions, replacements, and deletions are applied to the original remote directories. Unchanged sessions and successfully applied sessions are deleted automatically; declined or failed changes are retained for inspection and stale sessions are cleaned after seven days.

## Build

Build the x64 release package with:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64
```

The build creates `build\bin\x64_Release\sftpplug.wfx` and refreshes the ignored `SFTP\` deployment directory:

```text
SFTP\
  SFTPplug.wfx64
  SftpArchiveRouter.exe
  SFTPplug.chm
  sftp.php
  language\zh-cn.lng
```

The plugin and its static dependencies use `/MT`; no VC++ Redistributable or external DLL is required.

## Installation

With portable Total Commander stopped, mirror `SFTP\` to `<Total Commander>\Plugins\Wfx\SFTP\`. Register `SFTPplug.wfx64` as the SFTP filesystem plugin if needed. Run `SftpArchiveRouter.exe init` from the deployed plugin directory, then start Total Commander.

Do not add `SFTP\` to Git. It is generated by the release build.
