# Total Commander Secure FTP 插件

[English README](README.md)

这是一个现代 C++20 x64 Total Commander 文件系统插件。它支持 SSH/SFTP、SCP、Shell fallback、PHP Agent 和 LAN Pair 连接。

## 功能

- 基于非阻塞 libssh2 I/O 的 SFTP 传输，支持断点续传。
- 面向受限 SSH 服务器的 SCP 和 Shell fallback，使用 `cat`、`dd` 或 `base64`。
- 支持通过已保存 session 的 ProxyJump、HTTP CONNECT、SOCKS4/4a/5、IPv4 和 IPv6。
- 支持密码、keyboard-interactive、SSH agent、PEM/OpenSSH 和原生 PPK v2/v3 认证。
- 集成 Windows DPAPI 和 Total Commander master password。
- 通过随附的 `sftp.php` 支持 PHP Agent 和 PHP Shell。
- LAN Pair 支持 Windows 到 Windows 的直连传输。
- 支持从 PuTTY、WinSCP 和 KiTTY 导入 session。
- 提供简体中文外部翻译，英文为编译内置 fallback。

## 架构

```text
Total Commander WFX API
  -> 插件入口点和异常屏障
  -> 连接、认证和 session registry
  -> SFTP、SCP、Shell、PHP Agent 或 LAN Pair 传输后端
  -> libssh2 / WinHTTP / Windows 网络 API
```

`DllExceptionBarrier` 会保护每个导出的 `Fs*` 函数，避免 C++ 异常穿过 Total Commander ABI。

`ServerRegistry` 持有活动 session。跨 session 文件复制会在活动 SFTP handle 之间流式传输数据。Archive pipe 使用 session lease，确保 router 操作正在使用 SSH 连接时，该连接不会被关闭。

## SFTP TAR Router

`SftpArchiveRouter.exe` 必须和 `SFTPplug.wfx64` 放在同一个目录。Router 负责本地 `tar.exe` 和 `7z.exe` 相关工作；插件通过 named pipe 使用当前活动 SSH session 和远端 TAR 命令。

便携版 Total Commander 部署后，应在 Total Commander 关闭时，从插件目录执行一次：

```powershell
SftpArchiveRouter.exe init
```

自动化部署时使用：

```powershell
SftpArchiveRouter.exe init -y
```

`init` 会注册快捷键和 user command，并写入 `%COMMANDER_PATH%` 形式的路径，因此整个便携目录移动后仍然可用。

| 快捷键 | 功能 |
| --- | --- |
| `Alt+F5` | 创建保留的 `.tar` 归档。 |
| `Alt+F6` | 解压一个选中的归档。 |
| `Alt+F7` | 当任一面板是 SFTP 时，执行加速 TAR copy。 |
| `Alt+F8` | 当任一面板是 SFTP 时，执行加速 TAR move；只有目标成功后才删除源。 |
| `Alt+F9` | 对选中的 SFTP 文件和目录执行远端批量删除。 |
| `Alt+F11` | 预热当前 SFTP 目录树，加速 Total Commander 原生同步目录比较。 |
| `Alt+F12` | 将 SFTP 面板目录镜像到本地临时目录，并打开 Total Commander 本地 Synchronize Directories 窗口。 |
| `Ctrl+P` | 管理一个 SFTP profile 的 SSH Local、Remote 和 Dynamic SOCKS5 隧道。 |

Router 会在每次 SFTP TAR 操作前弹出目标输入框。Pack 默认目标是生成的 `.tar` 文件名；copy、move 和 unpack 默认目标是目标目录。按 Enter 使用默认值，也可以编辑目标后继续。原生 `F5` 和 `F6` 不会被覆盖。TAR streaming 需要活动 SSH/SFTP session 和远端 `tar`；PHP Agent 和 LAN Pair 不支持该路径。部署包自带 `Alt+F6` 所需的 `7z.exe` 运行时；`PATH` 或 `C:\Program Files\7-Zip` 中的安装仅作为后备。

## SSH 隧道

连接设置里的 **Tunnels...** 可以手写一行一条规则。`+` 表示主 SSH/SFTP 面板 session 连接后自动启动；`-` 表示保存但保持关闭。后台传输 session 不会持有隧道 listener。

`Ctrl+P` 会打开当前 profile 的隧道管理器，可以在不重连的情况下新增、编辑、删除、启动和停止规则。profile 已连接时显示真实的 `Running`、`Stopped` 或 `Failed` 状态；未连接时显示下次连接使用的 `Enabled` 或 `Disabled` 保存状态。窗口会明确提示 Toggle 是立即应用，还是在下次连接时应用。在线修改会立即作用于当前主 session，并写回配置供重连继续沿用。

`Ctrl+P` 的 **Add...** 窗口内置三类模板：

```text
- -L 0.0.0.0:2260:127.0.0.1:2260
- -R 0.0.0.0:1080:127.0.0.1:1080
- -D 0.0.0.0:1081
```

Profile 不会自动获得这些规则。需要哪条就点 **Add...** 按模板添加；很多时候模板不需要修改，直接保存即可。

规则格式示例：

```text
+ -L 127.0.0.1:8080:app.internal:80
- -R 0.0.0.0:2222:127.0.0.1:22
+ -D [::1]:1080
```

`-L` 和 `-R` 使用：

```text
[bind_address:]listen_port:target_host:target_port
```

`-D` 使用：

```text
[bind_address:]listen_port
```

IPv6 地址需要加方括号，例如 `[::1]`。

`-L` 和 `-D` 的 listener 位于运行 Total Commander 的本机。`-R` 的 listener 位于 SSH 服务器，连接到它的流量会通过当前 SSH session 转发回 Total Commander 本机看到的 `target_host:target_port`。远端是否允许指定监听地址取决于 SSH 服务器配置。成功启动、停止和启动失败都会写入 Total Commander 日志。

## 隧道 Smoke Test

部署 Release build 后，可使用一个 Unix SSH/SFTP profile 和一个 Windows OpenSSH/SFTP profile 验证真实隧道流量：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\tunnel-smoke.ps1 -TotalCommanderPath '<Total Commander>' -UnixSession 'vps' -WindowsSession 'win@'
```

该测试会打开两个 WFX session，临时替换每个 profile 的规则，并用真实 TCP echo 流量验证 `-L`、`-D` 和 `-R`。覆盖 manager Toggle、Enabled/Disabled 重连持久化、启动失败与重试、活动 relay 关闭、多规则独立性，以及 Disable 后本地和远端 listener 确实关闭。原规则会在 `finally` 中恢复。Unix 远端需要 Python 3，Windows OpenSSH 远端需要 PowerShell。

`0.0.0.0` 表示监听所有网卡，允许其它设备通过这台机器的 IP 访问；只想允许本机访问时应使用 `127.0.0.1`。Remote listener 是否允许 `0.0.0.0` 还取决于 SSH 服务器配置，例如 `GatewayPorts`。

隧道启用状态只由主窗口的 `Toggle` 控制。当前 profile 已连接时，Toggle 会立即启用或停止隧道，并保存为下次连接的默认状态；未连接时，Toggle 只保存状态，在下次连接时应用。

SSH keepalive 等价于：

```text
ServerAliveInterval=30
ServerAliveCountMax=120
TCPKeepAlive=yes
```

## 目录预热与本地比较

`Alt+F11` 会对当前 SFTP 目录树执行一次远端 `find`，并在内存中缓存目录项名称、大小和修改时间十分钟。Total Commander 仍然通过正常 WFX API 执行同步目录比较、补丁选择和复制操作。远端写入、重命名、删除、移动或断开连接后，缓存会失效。

`Alt+F12` 和 SFTP 元数据缓存相互独立。它会把 SFTP 面板目录作为 TAR stream 下载到 `%TEMP%\SftpLocalDiff\<session>`，然后打开 Total Commander 的本地 Synchronize Directories 窗口。窗口关闭后，来自 SFTP mirror 的更改会先汇总并要求明确确认，之后才应用到原始远端目录。无变化和成功应用的 session 会自动删除；用户拒绝或失败的 session 会保留以便检查，过期 session 会在七天后清理。

## 构建

构建 x64 Release 包：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' build\SFTPplug.sln /m /p:Configuration=Release /p:Platform=x64
```

构建会生成 `build\bin\x64_Release\sftpplug.wfx`，并刷新被 Git 忽略的 `SFTP\` 部署目录：

```text
SFTP\
  SFTPplug.wfx64
  SftpArchiveRouter.exe
  SFTPplug.chm
  sftp.php
  language\zh-cn.lng
  7z.exe
  7z.dll
  7zip-License.txt
```

插件和静态依赖使用 `/MT`，不需要额外安装 VC++ Redistributable 或外部 DLL。

## 安装

关闭便携版 Total Commander 后，将生成的 `SFTP\` 目录同步到：

```text
<Total Commander>\Plugins\Wfx\SFTP\
```

如有需要，将 `SFTPplug.wfx64` 注册为 SFTP 文件系统插件。然后在部署后的插件目录执行：

```powershell
SftpArchiveRouter.exe init
```

## 更新便携包

如果要将当前 Release 插件更新到一个干净的绿色版 Total Commander ZIP，使用：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\update-portable-package.ps1 -ArchivePath '<Total Commander ZIP>'
```

这与实例部署不同。脚本会临时解压 ZIP，只替换其中的 SFTP 插件目录，注册 router 快捷键和 user command，验证结果后原子替换原 ZIP，并删除临时目录。它不会启动干净的 Total Commander 实例；运行前请关闭所有正在运行的 Total Commander 实例。

自动化部署应使用：

```powershell
SftpArchiveRouter.exe init -y
```

不要把 `SFTP\` 目录加入 Git。它是 Release build 生成的部署产物。
