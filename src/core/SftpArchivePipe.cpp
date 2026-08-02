#include "global.h"
#include <windows.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "CoreUtils.h"
#include "SftpArchivePipe.h"
#include "SftpArchiveOpenRetry.h"
#include "SftpClient.h"
#include "SftpInternal.h"
#include "PluginEntryPoints.h"
#include "ServerRegistry.h"
#include "TransferUtils.h"
#include "UnicodeHelpers.h"

namespace {

constexpr DWORD kArchiveMagic = 0x41504653; // "SFPA"
constexpr DWORD kArchivePut = 1;
constexpr DWORD kArchiveGet = 2;
constexpr DWORD kArchivePack = 3;
constexpr DWORD kArchivePackToRemote = 4;
constexpr DWORD kArchiveExtract = 5;
constexpr DWORD kArchivePackExtractRemote = 6;
constexpr DWORD kArchiveIsDirectory = 7;
constexpr DWORD kArchiveDeleteRemote = 8;
constexpr DWORD kArchivePrewarmManifest = 9;
constexpr DWORD kArchiveLogStatus = 10;
constexpr DWORD kArchiveShowError = 11;
constexpr DWORD kArchiveTunnelStatus = 12;
constexpr DWORD kArchiveTunnelSetEnabled = 13;
constexpr DWORD kArchiveTunnelReplaceRules = 14;
constexpr DWORD kMaxPathBytes = 64 * 1024;
constexpr DWORD kMaxItemBytes = 1024 * 1024;
constexpr DWORD kChunkSize = 64 * 1024;
constexpr size_t kMaxManifestBytes = 64 * 1024 * 1024;
constexpr ULONGLONG kManifestLifetimeMs = 10 * 60 * 1000;
constexpr DWORD kArchiveCompletionTimeoutMs = 5 * 60 * 1000;
constexpr DWORD kArchiveIoStallTimeoutMs = 5 * 60 * 1000;

struct ArchiveRequest {
    DWORD magic;
    DWORD operation;
    DWORD sourcePathBytes;
    DWORD targetPathBytes;
    DWORD itemBytes;
};

std::atomic<bool> g_running = false;
std::thread g_listener;

struct ManifestCache {
    pConnectSettings cs = nullptr;
    std::string rootPath;
    ULONGLONG createdAt = 0;
    std::unordered_map<std::string, std::vector<WIN32_FIND_DATAW>> directories;
};

std::mutex g_manifestMutex;
std::unique_ptr<ManifestCache> g_manifestCache;

bool SaveSshTunnelRules(pConnectSettings cs, std::string& error)
{
    if (!cs || cs->DisplayName.empty() || cs->IniFileName.empty()) {
        error = "The active SFTP session cannot save tunnel rules.";
        return false;
    }
    for (unsigned index = 1; index <= 64; ++index) {
        const std::string key = std::format("tunnel{}", index);
        WritePrivateProfileString(cs->DisplayName.c_str(), key.c_str(), nullptr, cs->IniFileName.c_str());
    }
    for (size_t index = 0; index < cs->sshTunnels.size() && index < 64; ++index) {
        const std::string key = std::format("tunnel{}", index + 1);
        WritePrivateProfileString(cs->DisplayName.c_str(), key.c_str(), FormatSshTunnelRule(cs->sshTunnels[index]).c_str(), cs->IniFileName.c_str());
    }
    return true;
}

bool ParseTunnelRuleLines(std::string_view text, std::vector<SshTunnelRule>& rules, std::string& error)
{
    rules.clear();
    for (size_t start = 0; start < text.size();) {
        const size_t end = text.find('\n', start);
        std::string line(text.substr(start, (end == std::string_view::npos ? text.size() : end) - start));
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (!line.empty()) {
            SshTunnelRule rule;
            if (!ParseSshTunnelRule(line, rule, error))
                return false;
            rules.push_back(std::move(rule));
            if (rules.size() > 64) {
                error = "A session can contain at most 64 SSH tunnel rules.";
                return false;
            }
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return ValidateSshTunnelRules(rules, error);
}

bool ReadExact(HANDLE pipe, void* data, DWORD length)
{
    auto* out = static_cast<BYTE*>(data);
    while (length > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, out, length, &got, nullptr) || got == 0)
            return false;
        out += got;
        length -= got;
    }
    return true;
}

bool WriteExact(HANDLE pipe, const void* data, DWORD length)
{
    const auto* input = static_cast<const BYTE*>(data);
    while (length > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, input, length, &written, nullptr) || written == 0)
            return false;
        input += written;
        length -= written;
    }
    return true;
}

void SendResult(HANDLE pipe, int result, const std::string& text)
{
    const DWORD code = static_cast<DWORD>(result);
    const DWORD textBytes = static_cast<DWORD>((std::min)(text.size(), static_cast<size_t>(4096)));
    WriteExact(pipe, &code, sizeof(code));
    WriteExact(pipe, &textBytes, sizeof(textBytes));
    if (textBytes)
        WriteExact(pipe, text.data(), textBytes);
}

bool ReadText(HANDLE pipe, DWORD length, std::string& text)
{
    if (length > kMaxItemBytes)
        return false;
    text.assign(length, '\0');
    return length == 0 || ReadExact(pipe, text.data(), length);
}

bool ParseSftpPath(const std::string& virtualPath, std::string& sessionName, std::wstring& relativePath)
{
    const size_t start = virtualPath.find_first_not_of("\\/");
    if (start == std::string::npos)
        return false;
    const size_t rootEnd = virtualPath.find_first_of("\\/", start);
    if (rootEnd == std::string::npos || rootEnd - start != 4 ||
        _strnicmp(virtualPath.c_str() + start, "SFTP", 4) != 0)
        return false;
    const size_t sessionStart = virtualPath.find_first_not_of("\\/", rootEnd);
    if (sessionStart == std::string::npos)
        return false;
    const size_t sessionEnd = virtualPath.find_first_of("\\/", sessionStart);
    sessionName = sessionEnd == std::string::npos
        ? virtualPath.substr(sessionStart)
        : virtualPath.substr(sessionStart, sessionEnd - sessionStart);
    if (sessionName.empty())
        return false;
    const std::string relative = sessionEnd == std::string::npos ? "\\" : virtualPath.substr(sessionEnd);
    relativePath = unicode_util::utf8_to_wstring(relative);
    return !relativePath.empty();
}

struct RemoteEndpoint {
    ServerSessionLease lease;
    pConnectSettings cs = nullptr;
    std::string remotePath;
};

bool OpenEndpoint(const std::string& virtualPath, RemoteEndpoint& endpoint, std::string& error)
{
    std::string sessionName;
    std::wstring relativePath;
    if (!ParseSftpPath(virtualPath, sessionName, relativePath)) {
        error = "Invalid SFTP path.";
        return false;
    }
    endpoint.lease = AcquireServerSessionLease(sessionName.c_str());
    endpoint.cs = static_cast<pConnectSettings>(endpoint.lease.get());
    if (!endpoint.cs) {
        error = "The SFTP session is not connected.";
        return false;
    }
    if (IsPhpAgentTransport(endpoint.cs) || IsLanPairTransport(endpoint.cs) || !endpoint.cs->session) {
        error = "The selected transport does not support SSH archive streaming.";
        return false;
    }
    // Panel operations can reconnect after a TCP loss while an archive request
    // still holds the previous session object. Check it before opening tar.
    if (!ReconnectSFTPChannelIfNeeded(endpoint.cs)) {
        error = "The SFTP session could not be reconnected for archive streaming.";
        return false;
    }
    endpoint.remotePath = ToRemotePathA(endpoint.cs, relativePath.c_str());
    while (endpoint.remotePath.size() > 1 && endpoint.remotePath.ends_with("/."))
        endpoint.remotePath.resize(endpoint.remotePath.size() - 2);
    while (endpoint.remotePath.size() > 1 && endpoint.remotePath.ends_with('/'))
        endpoint.remotePath.pop_back();
    return !endpoint.remotePath.empty();
}

bool ReconnectArchiveSession(pConnectSettings cs)
{
    if (!cs || IsPhpAgentTransport(cs) || IsLanPairTransport(cs))
        return false;
    SftpCloseConnection(cs);
    if (SftpConnect(cs) != SFTP_OK || !cs->session || (!cs->scponly && !cs->sftpsession))
        return false;
    StartSshSessionServices(cs);
    return true;
}

bool WaitForArchiveIo(pConnectSettings cs)
{
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (EscapePressed())
            return false;
        if (WaitForSshIo(cs, SOCKET_READ_POLL_MS))
            return true;
        if (cs && !cs->transport_stream && IsSocketDisconnected(cs->sock))
            return false;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= kArchiveIoStallTimeoutMs)
            return false;
    }
}

bool WriteRemoteAll(ISshChannel* channel, pConnectSettings cs, const char* data, size_t length,
                    std::string& error)
{
    while (length > 0) {
        ssize_t rc = 0;
        {
            ScopedSshSessionUse sessionUse(cs);
            rc = channel->write(data, length);
        }
        if (rc > 0) {
            data += rc;
            length -= static_cast<size_t>(rc);
            continue;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            // No socket event in one short poll is normal under load. Keep
            // retrying until the archive connection has genuinely stalled.
            if (!WaitForArchiveIo(cs)) {
                error = "The remote archive stream stalled before all data was sent.";
                return false;
            }
            continue;
        }
        error = "Could not write archive data to the remote SSH channel (SSH error " + std::to_string(rc) + ").";
        return false;
    }
    return true;
}

std::unique_ptr<ISshChannel> StartCommand(pConnectSettings cs, const std::string& command, std::string& error)
{
    const auto start = [&]() -> std::unique_ptr<ISshChannel> {
        std::unique_ptr<ISshChannel> channel;
        {
            ScopedSshSessionUse sessionUse(cs);
            channel = ConnectChannel(cs->session.get(), cs->sock);
        }
        if (!channel || WaitForOperation([&] {
                ScopedSshSessionUse sessionUse(cs);
                return channel->exec(command.c_str());
            }, SSH_AUTH_STAGE_TIMEOUT_MS, cs) < 0)
            return {};
        return channel;
    };

    if (auto channel = start())
        return channel;

    // A dropped TCP connection can leave libssh2 allocated until its first
    // channel operation fails. Retry only before acknowledging the router,
    // so no archive bytes can be sent twice.
    if (ReconnectArchiveSession(cs)) {
        if (auto channel = start())
            return channel;
    }
    error = "Could not start the remote archive command after reconnecting the SFTP session.";
    return {};
}

void DrainCommandStderr(ISshChannel* channel, pConnectSettings cs, std::string& stderrText)
{
    std::array<char, 2048> stderrData{};
    for (;;) {
        ssize_t rc = 0;
        {
            ScopedSshSessionUse sessionUse(cs);
            rc = channel->readStderr(stderrData.data(), stderrData.size());
        }
        if (rc <= 0)
            return;
        stderrText.append(stderrData.data(), static_cast<size_t>(rc));
    }
}

bool CloseCommand(std::unique_ptr<ISshChannel>& channel, pConnectSettings cs, bool sendEof,
                   std::string& error)
{
    if (!channel)
        return false;

    int flushResult = 0;
    int sendEofResult = 0;
    int waitEofResult = 0;
    if (sendEof) {
        // libssh2 can return EAGAIN from sendEof while prior channel writes
        // remain buffered. Flush them first, then close the remote stdin.
        flushResult = WaitForOperation([&] {
            ScopedSshSessionUse sessionUse(cs);
            return channel->flush();
        }, kArchiveCompletionTimeoutMs, cs);
        if (flushResult >= 0) {
            sendEofResult = WaitForOperation([&] {
                ScopedSshSessionUse sessionUse(cs);
                return channel->sendEof();
            }, kArchiveCompletionTimeoutMs, cs);
        }
    }
    std::string stderrText;
    if (flushResult >= 0 && sendEofResult >= 0) {
        const auto eofStart = std::chrono::steady_clock::now();
        for (;;) {
            // A remote tar can emit enough diagnostics to fill SSH stderr before
            // it reaches EOF. Drain it here so waitEof cannot deadlock.
            DrainCommandStderr(channel.get(), cs, stderrText);
            {
                ScopedSshSessionUse sessionUse(cs);
                waitEofResult = channel->waitEof();
            }
            if (waitEofResult != LIBSSH2_ERROR_EAGAIN)
                break;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - eofStart).count();
            if (elapsed >= kArchiveCompletionTimeoutMs || !WaitForArchiveIo(cs))
                break;
        }
    }
    DrainCommandStderr(channel.get(), cs, stderrText);
    int exitStatus = 1;
    {
        ScopedSshSessionUse sessionUse(cs);
        exitStatus = channel->getExitStatus();
    }
    WaitForOperation([&] {
        ScopedSshSessionUse sessionUse(cs);
        return channel->channelClose();
    }, SSH_AUTH_STAGE_TIMEOUT_MS, cs);
    WaitForOperation([&] {
        ScopedSshSessionUse sessionUse(cs);
        return channel->channelFree();
    }, SSH_AUTH_STAGE_TIMEOUT_MS, cs);
    channel.reset();

    const int sessionError = cs && cs->session ? cs->session->lastErrno() : 0;
    SFTP_LOG("ARCHIVE", "close flush=%d sendEof=%d waitEof=%d exit=%d sessionError=%d stderr='%s'",
             flushResult, sendEofResult, waitEofResult, exitStatus, sessionError, stderrText.c_str());
    const auto appendError = [&](const std::string& detail) {
        if (error.empty())
            error = detail;
        else if (!detail.empty())
            error += "\n" + detail;
    };
    if (exitStatus != 0) {
        appendError(stderrText.empty()
            ? "The remote archive command failed (exit status " + std::to_string(exitStatus) + ")."
            : stderrText);
        return false;
    }
    if (flushResult < 0) {
        appendError("Could not flush remote archive input (SSH error " + std::to_string(flushResult) + ").");
        appendError(stderrText);
        return false;
    }
    if (sendEofResult < 0) {
        appendError("Could not finish remote archive input (SSH error " + std::to_string(sendEofResult) + ").");
        appendError(stderrText);
        return false;
    }
    if (waitEofResult < 0) {
        appendError("The remote archive command did not finish (SSH error " + std::to_string(waitEofResult) + ").");
        appendError(stderrText);
        return false;
    }
    return true;
}

bool ReadRemoteChunk(ISshChannel* channel, pConnectSettings cs, char* buffer, size_t length,
                     ssize_t& count, std::string& error)
{
    for (;;) {
        {
            ScopedSshSessionUse sessionUse(cs);
            count = channel->read(buffer, length);
        }
        if (count >= 0)
            return true;
        if (count != LIBSSH2_ERROR_EAGAIN) {
            error = "Could not read the remote archive stream.";
            return false;
        }
        if (!WaitForArchiveIo(cs)) {
            error = "The remote archive stream stalled.";
            return false;
        }
    }
}

std::string Quote(const std::string& text)
{
    return "'" + string_util::ShellQuoteSingle(text) + "'";
}

bool IsWindowsRemotePath(const std::string& path)
{
    return path.size() >= 3 && path[0] == '/' &&
        ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z')) && path[2] == ':';
}

std::wstring ToWindowsPath(const std::string& path)
{
    std::wstring result = unicode_util::utf8_to_wstring(path);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    if (!result.empty() && result.front() == L'\\')
        result.erase(result.begin());
    return result;
}

std::wstring QuotePowerShell(const std::wstring& text)
{
    std::wstring quoted(L"'");
    for (const wchar_t character : text) {
        if (character == L'\'')
            quoted += L"''";
        else
            quoted.push_back(character);
    }
    quoted.push_back(L'\'');
    return quoted;
}

std::string EncodePowerShellCommand(const std::wstring& script)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string bytes;
    bytes.reserve(script.size() * sizeof(wchar_t));
    for (const wchar_t character : script) {
        bytes.push_back(static_cast<char>(character & 0xFF));
        bytes.push_back(static_cast<char>(character >> 8));
    }
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t offset = 0; offset < bytes.size(); offset += 3) {
        const unsigned value = static_cast<unsigned char>(bytes[offset]) << 16 |
            (offset + 1 < bytes.size() ? static_cast<unsigned char>(bytes[offset + 1]) << 8 : 0) |
            (offset + 2 < bytes.size() ? static_cast<unsigned char>(bytes[offset + 2]) : 0);
        encoded.push_back(alphabet[(value >> 18) & 0x3F]);
        encoded.push_back(alphabet[(value >> 12) & 0x3F]);
        encoded.push_back(offset + 1 < bytes.size() ? alphabet[(value >> 6) & 0x3F] : '=');
        encoded.push_back(offset + 2 < bytes.size() ? alphabet[value & 0x3F] : '=');
    }
    // Prevent PowerShell from consuming the SSH channel as pipeline input.
    // Archive data is read explicitly through Console.OpenStandardInput().
    return "powershell.exe -NoProfile -NonInteractive -InputFormat None -EncodedCommand " + encoded;
}

std::wstring QuoteWindowsArgument(const std::wstring& text)
{
    std::wstring quoted(L"\"");
    size_t backslashes = 0;
    for (const wchar_t character : text) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
            quoted.append(backslashes * 2 + 1, L'\\');
        else
            quoted.append(backslashes, L'\\');
        quoted.push_back(character);
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::string QuoteCmdArgument(const std::wstring& text)
{
    std::string quoted("\"");
    for (const wchar_t character : text) {
        if (character == L'\"')
            quoted += "\\\"";
        else {
            char encoded[4]{};
            const int length = WideCharToMultiByte(CP_UTF8, 0, &character, 1, encoded, sizeof(encoded), nullptr, nullptr);
            quoted.append(encoded, length > 0 ? static_cast<size_t>(length) : 0);
        }
    }
    quoted += "\"";
    return quoted;
}

bool ReadCommandOutput(std::unique_ptr<ISshChannel>& channel, pConnectSettings cs,
                       std::string& output, std::string& error)
{
    std::array<char, kChunkSize> buffer{};
    output.clear();
    for (;;) {
        ssize_t count = 0;
        if (!ReadRemoteChunk(channel.get(), cs, buffer.data(), buffer.size(), count, error))
            return false;
        if (count == 0)
            return true;
        if (output.size() + static_cast<size_t>(count) > kMaxManifestBytes) {
            error = "The remote directory manifest exceeds 64 MiB.";
            return false;
        }
        output.append(buffer.data(), static_cast<size_t>(count));
    }
}

bool ParseManifestOutput(const std::string& output, const RemoteEndpoint& endpoint,
                          std::unique_ptr<ManifestCache>& cache, std::string& error)
{
    auto parsed = std::make_unique<ManifestCache>();
    parsed->cs = endpoint.cs;
    parsed->rootPath = endpoint.remotePath;
    parsed->createdAt = GetTickCount64();
    size_t offset = 0;
    while (offset < output.size()) {
        std::array<std::string_view, 4> fields{};
        for (auto& field : fields) {
            const size_t end = output.find('\0', offset);
            if (end == std::string::npos) {
                error = "The remote directory manifest is incomplete.";
                return false;
            }
            field = std::string_view(output.data() + offset, end - offset);
            offset = end + 1;
        }
        if (fields[0].size() != 1 || (fields[0][0] != 'd' && fields[0][0] != 'f' && fields[0][0] != 'l'))
            continue;
        if (fields[3] == "." || fields[3].empty()) {
            parsed->directories.try_emplace(endpoint.remotePath);
            continue;
        }
        const size_t slash = fields[3].find_last_of('/');
        const std::string parent = slash == std::string_view::npos
            ? endpoint.remotePath
            : endpoint.remotePath + (endpoint.remotePath.ends_with('/') ? "" : "/") +
                std::string(fields[3].substr(0, slash));
        const std::string_view name = slash == std::string_view::npos ? fields[3] : fields[3].substr(slash + 1);
        if (name.empty())
            continue;
        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
        if (wideLength <= 0 || wideLength >= MAX_PATH)
            continue;
        WIN32_FIND_DATAW data{};
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), data.cFileName, wideLength);
        data.cFileName[wideLength] = L'\0';
        if (fields[0][0] == 'd') {
            data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            parsed->directories.try_emplace(parent + (parent.ends_with('/') ? "" : "/") + std::string(name));
        } else {
            const unsigned long long size = _strtoui64(std::string(fields[1]).c_str(), nullptr, 10);
            data.nFileSizeHigh = static_cast<DWORD>(size >> 32);
            data.nFileSizeLow = static_cast<DWORD>(size);
        }
        const time_t modified = static_cast<time_t>(strtoll(std::string(fields[2]).c_str(), nullptr, 10));
        ConvUnixTimeToFileTime(&data.ftLastWriteTime, modified);
        parsed->directories[parent].push_back(data);
    }
    cache = std::move(parsed);
    return true;
}

bool PrewarmManifest(const std::string& sourcePath, std::string& error)
{
    RemoteEndpoint endpoint;
    if (!OpenEndpoint(sourcePath, endpoint, error))
        return false;
    std::string command;
    if (IsWindowsRemotePath(endpoint.remotePath)) {
        const std::wstring root = ToWindowsPath(endpoint.remotePath);
        const std::wstring script =
            L"$root=(Get-Item -LiteralPath " + QuotePowerShell(root) + L").FullName.TrimEnd('\\');"
            L"$output=[Console]::OpenStandardOutput();"
            L"$entries=@(Get-Item -LiteralPath $root)+@(Get-ChildItem -LiteralPath $root -Recurse -Force);"
            L"foreach($entry in $entries){"
                L"$type=if($entry.PSIsContainer){'d'}elseif($entry.Attributes -band [IO.FileAttributes]::ReparsePoint){'l'}else{'f'};"
                L"$size=if($entry.PSIsContainer){0}else{$entry.Length};"
                L"$time=([DateTimeOffset]$entry.LastWriteTimeUtc).ToUnixTimeSeconds();"
                L"$name=if($entry.FullName -eq $root){'.'}else{$entry.FullName.Substring($root.Length).TrimStart('\\').Replace('\\','/')};"
                L"$bytes=[Text.Encoding]::UTF8.GetBytes(\"$type`0$size`0$time`0$name`0\");$output.Write($bytes,0,$bytes.Length)"
            L"}";
        command = EncodePowerShellCommand(script);
    } else {
        command = "find " + Quote(endpoint.remotePath) + " -printf '%y\\000%s\\000%T@\\000%P\\000'";
    }
    auto channel = StartCommand(endpoint.cs, command, error);
    if (!channel)
        return false;
    std::string output;
    const bool readOk = ReadCommandOutput(channel, endpoint.cs, output, error);
    const bool closeOk = CloseCommand(channel, endpoint.cs, false, error);
    if (!readOk || !closeOk)
        return false;
    std::unique_ptr<ManifestCache> cache;
    if (!ParseManifestOutput(output, endpoint, cache, error))
        return false;
    std::lock_guard<std::mutex> lock(g_manifestMutex);
    g_manifestCache = std::move(cache);
    return true;
}

std::string BuildRemotePackCommand(const RemoteEndpoint& source, const std::string& itemList)
{
    if (IsWindowsRemotePath(source.remotePath)) {
        std::string command = "tar.exe -cf - -C " + QuoteCmdArgument(ToWindowsPath(source.remotePath)) + " --";
        if (itemList.empty())
            return command + " .";
        size_t start = 0;
        while (start < itemList.size()) {
            const size_t end = itemList.find('\n', start);
            const std::string item = itemList.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!item.empty())
                command += " " + QuoteCmdArgument(unicode_util::utf8_to_wstring(item));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return command;
    }
    std::string command = "tar -cf - -C " + Quote(source.remotePath) + " --";
    if (itemList.empty())
        return command + " .";
    size_t start = 0;
    while (start < itemList.size()) {
        const size_t end = itemList.find('\n', start);
        const std::string item = itemList.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty())
            command += " " + Quote(item);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return command;
}

std::string BuildRemoteDeleteCommand(const RemoteEndpoint& source, const std::string& itemList)
{
    if (IsWindowsRemotePath(source.remotePath)) {
        std::wstring script;
        const std::wstring root = QuotePowerShell(ToWindowsPath(source.remotePath));
        size_t start = 0;
        while (start < itemList.size()) {
            const size_t end = itemList.find('\n', start);
            const std::string item = itemList.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!item.empty())
                script += L"Remove-Item -LiteralPath (Join-Path -Path " + root + L" -ChildPath " +
                    QuotePowerShell(unicode_util::utf8_to_wstring(item)) + L") -Recurse -Force -ErrorAction Stop;";
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return EncodePowerShellCommand(script);
    }
    std::string command = "cd " + Quote(source.remotePath) + " && rm -rf --";
    size_t start = 0;
    while (start < itemList.size()) {
        const size_t end = itemList.find('\n', start);
        const std::string item = itemList.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty())
            command += " " + Quote(item);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return command;
}

std::string RemoteParentPath(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

std::string BuildRemoteReceiveCommand(const RemoteEndpoint& target, bool extract)
{
    if (IsWindowsRemotePath(target.remotePath)) {
        const std::wstring targetPath = ToWindowsPath(target.remotePath);
        if (!extract)
            return {};
        // tar.exe reads the SSH channel directly. PowerShell's standard-input
        // wrapper can stall after large archive streams on Windows OpenSSH.
        return "tar.exe -xf - -C " + QuoteCmdArgument(targetPath);
    }
    const std::string parent = RemoteParentPath(target.remotePath);
    // Use only POSIX shell features to create an exclusive temporary file.
    // mktemp is common, but not required by every minimal remote installation.
    std::string command = "dir=" + Quote(parent) + "; temp=\"$dir/.sftp-archive.$$\"; n=0; "
        "while ! (umask 077; set -C; : > \"$temp\") 2>/dev/null; do "
        "n=$((n + 1)); [ \"$n\" -lt 16 ] || exit 1; temp=\"$dir/.sftp-archive.$$.$n\"; done; "
        "trap 'rm -f \"$temp\"' 0; "
        "trap 'rm -f \"$temp\"; exit 1' HUP INT TERM; "
        "cat > \"$temp\" || exit 1; ";
    if (extract) {
        // A complete stream is staged before extraction, and this listing
        // check rejects truncated TAR data before it can change the target.
        command += "tar -tf \"$temp\" >/dev/null || exit 1; "
            "tar -xf \"$temp\" -C " + Quote(target.remotePath) + "; ";
    } else {
        command += "mv -f \"$temp\" " + Quote(target.remotePath) + "; ";
    }
    command += "exit $?";
    return command;
}

std::string BuildRemoteReadFileCommand(const RemoteEndpoint& source)
{
    if (!IsWindowsRemotePath(source.remotePath))
        return "cat " + Quote(source.remotePath);
    return EncodePowerShellCommand(
        L"$file=[IO.File]::OpenRead(" + QuotePowerShell(ToWindowsPath(source.remotePath)) + L");"
        L"try{$file.CopyTo([Console]::OpenStandardOutput())}finally{$file.Dispose()}");
}

bool StreamProducerToPipe(HANDLE pipe, RemoteEndpoint& source, const std::string& command, std::string& error)
{
    // libssh2 keeps EAGAIN/block-direction state on the session, so a complete
    // archive stream must not interleave with panel enumeration on that session.
    ScopedSshSessionUse archiveUse(source.cs);
    auto channel = StartCommand(source.cs, command, error);
    if (!channel)
        return false;
    // The router must not start its local consumer until the remote producer is ready.
    SendResult(pipe, 0, "");
    std::array<char, kChunkSize> buffer{};
    bool ok = true;
    for (;;) {
        ssize_t count = 0;
        if (!ReadRemoteChunk(channel.get(), source.cs, buffer.data(), buffer.size(), count, error)) {
            ok = false;
            break;
        }
        if (count == 0)
            break;
        const DWORD length = static_cast<DWORD>(count);
        if (!WriteExact(pipe, &length, sizeof(length)) || !WriteExact(pipe, buffer.data(), length)) {
            error = "Archive receiver disconnected.";
            ok = false;
            break;
        }
    }
    // The router's local tar consumer must see a stream terminator before the
    // final result, including when the remote producer failed mid-stream.
    const DWORD endMarker = 0;
    if (!WriteExact(pipe, &endMarker, sizeof(endMarker))) {
        error = "Archive receiver disconnected.";
        ok = false;
    }
    return CloseCommand(channel, source.cs, false, error) && ok;
}

bool DrainArchiveSender(HANDLE pipe)
{
    std::array<char, kChunkSize> buffer{};
    for (;;) {
        DWORD length = 0;
        if (!ReadExact(pipe, &length, sizeof(length)))
            return false;
        if (length == 0)
            return true;
        if (length > buffer.size() || !ReadExact(pipe, buffer.data(), length))
            return false;
    }
}

bool StreamPipeToConsumer(HANDLE pipe, RemoteEndpoint& target, const std::string& command, std::string& error)
{
    // The scoped calls below are recursive and remain valid while this guard
    // prevents another WFX operation from changing libssh2 session state.
    ScopedSshSessionUse archiveUse(target.cs);
    auto channel = StartCommand(target.cs, command, error);
    if (!channel)
        return false;
    // The router must not start its local producer until the remote consumer is ready.
    SendResult(pipe, 0, "");
    std::array<char, kChunkSize> buffer{};
    bool ok = true;
    bool drainSender = false;
    for (;;) {
        DWORD length = 0;
        if (!ReadExact(pipe, &length, sizeof(length))) {
            error = "Archive sender disconnected.";
            ok = false;
            break;
        }
        if (length == 0)
            break;
        if (length > buffer.size() || !ReadExact(pipe, buffer.data(), length)) {
            error = "Archive sender disconnected.";
            ok = false;
            break;
        }
        if (!WriteRemoteAll(channel.get(), target.cs, buffer.data(), length, error)) {
            ok = false;
            // The router is still writing tar output. Drain it before sending
            // the final result so neither side blocks on a full pipe buffer.
            drainSender = true;
            break;
        }
    }
    // Drain before closing a failed remote channel. CloseCommand can wait for
    // an SSH timeout after a disconnect, while tar.exe is still writing.
    // Reading through the end marker releases tar.exe and avoids a pipe cycle.
    if (drainSender)
        DrainArchiveSender(pipe);
    const bool closeOk = CloseCommand(channel, target.cs, true, error);
    return closeOk && ok;
}

bool RemoveWindowsArchiveTemp(RemoteEndpoint& target, const std::string& temporaryPath)
{
    if (!target.cs || !target.cs->sftpsession)
        return false;
    for (;;) {
        int rc = 0;
        {
            ScopedSshSessionUse sessionUse(target.cs);
            rc = target.cs->sftpsession->unlink(temporaryPath.c_str());
        }
        if (rc != LIBSSH2_ERROR_EAGAIN)
            return rc == 0;
        if (!WaitForArchiveIo(target.cs))
            return false;
    }
}

bool OpenWindowsArchiveTemp(RemoteEndpoint& target, const std::string& temporaryPath,
                            std::unique_ptr<ISftpHandle>& output)
{
    if (!target.cs || !target.cs->sftpsession)
        return false;
    output = OpenSftpArchiveFileWithRetry(
        temporaryPath.c_str(), LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, 0600,
        [&](const char* path, unsigned long flags, long mode) {
            ScopedSshSessionUse sessionUse(target.cs);
            return target.cs->sftpsession->open(path, flags, mode);
        },
        [&] { return target.cs->session ? target.cs->session->lastErrno() : 0; },
        [&] { return WaitForArchiveIo(target.cs); });
    return !!output;
}

bool WriteWindowsArchiveTemp(RemoteEndpoint& target, ISftpHandle* output, const char* data, size_t length,
                             std::string& error)
{
    size_t written = 0;
    while (written < length) {
        ssize_t rc = 0;
        {
            ScopedSshSessionUse sessionUse(target.cs);
            rc = output->write(data + written, length - written);
        }
        if (rc > 0) {
            written += static_cast<size_t>(rc);
            continue;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN && WaitForArchiveIo(target.cs))
            continue;
        error = "Could not write the temporary Windows archive through SFTP.";
        return false;
    }
    return true;
}

bool CloseWindowsArchiveTemp(RemoteEndpoint& target, std::unique_ptr<ISftpHandle>& output)
{
    int closeResult = 0;
    while (output) {
        {
            ScopedSshSessionUse sessionUse(target.cs);
            closeResult = output->close();
        }
        if (closeResult != LIBSSH2_ERROR_EAGAIN)
            break;
        if (!WaitForArchiveIo(target.cs)) {
            closeResult = -1;
            break;
        }
    }
    output.reset();
    return closeResult == 0;
}

bool PublishWindowsArchiveTemp(RemoteEndpoint& target, const std::string& temporaryPath,
                               std::string& error)
{
    wchar_t systemDirectory[MAX_PATH]{};
    const UINT systemLength = GetSystemDirectoryW(systemDirectory, ARRAYSIZE(systemDirectory));
    const std::wstring ssh = systemLength && systemLength < ARRAYSIZE(systemDirectory)
        ? std::wstring(systemDirectory) + L"\\OpenSSH\\ssh.exe"
        : L"";
    if (ssh.empty() || GetFileAttributesW(ssh.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "Windows OpenSSH ssh.exe is required to publish the completed archive.";
        return false;
    }
    if (target.cs->user.empty() || target.cs->server.empty()) {
        error = "The Windows SFTP archive target has no SSH user or server.";
        return false;
    }

    const std::wstring temporary = QuotePowerShell(ToWindowsPath(temporaryPath));
    const std::wstring destination = QuotePowerShell(ToWindowsPath(target.remotePath));
    const std::string remoteCommand = EncodePowerShellCommand(
        L"$ErrorActionPreference='Stop';$deadline=[DateTime]::UtcNow.AddSeconds(10);$lastError='';do{try{"
        L"& tar.exe -tf " + temporary + L" *> $null;if($LASTEXITCODE -ne 0){throw 'The staged archive is invalid'};"
        L"if([IO.File]::Exists(" + destination + L")){[IO.File]::Replace(" + temporary + L"," + destination + L",$null)}else{"
        L"Move-Item -LiteralPath " + temporary + L" -Destination " + destination + L" -ErrorAction Stop};"
        L"if([IO.File]::Exists(" + destination + L") -and -not [IO.File]::Exists(" + temporary + L")){exit 0}}"
        L"catch{$lastError=$_.Exception.Message};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $deadline);"
        L"[Console]::Error.WriteLine($lastError);exit 1");
    std::array<char, 1024> hostBuffer{};
    strncpy_s(hostBuffer.data(), hostBuffer.size(), target.cs->server.c_str(), _TRUNCATE);
    WORD parsedPort = 22;
    if (!ParseAddress(hostBuffer.data(), hostBuffer.data(), &parsedPort, 22)) {
        error = "The Windows SFTP archive target has an invalid SSH server address.";
        return false;
    }
    std::wstring host = unicode_util::utf8_to_wstring(hostBuffer.data());
    if (host.find(L':') != std::wstring::npos && !(host.starts_with(L"[") && host.ends_with(L"]")))
        host = L"[" + host + L"]";
    const unsigned short port = target.cs->customport ? target.cs->customport : parsedPort;
    std::wstring command = QuoteWindowsArgument(ssh) +
        L" -n -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 -o ServerAliveCountMax=120" +
        L" -o TCPKeepAlive=yes -p " + std::to_wstring(port) + L" " +
        QuoteWindowsArgument(unicode_util::utf8_to_wstring(target.cs->user) + L"@" + host) + L" " +
        QuoteWindowsArgument(unicode_util::utf8_to_wstring(remoteCommand));
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stderrRead, &stderrWrite, &inheritable, 0)) {
        error = "Could not capture independent SSH archive publish errors.";
        return false;
    }
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullHandle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                    OPEN_EXISTING, 0, nullptr);
    if (nullHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);
        error = "Could not prepare independent SSH archive publish I/O.";
        return false;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullHandle;
    startup.hStdOutput = nullHandle;
    startup.hStdError = stderrWrite;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(ssh.c_str(), commandBuffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);
        CloseHandle(nullHandle);
        error = "Could not start the independent SSH archive publish command.";
        return false;
    }
    CloseHandle(stderrWrite);
    CloseHandle(nullHandle);
    const DWORD wait = WaitForSingleObject(process.hProcess, kArchiveCompletionTimeoutMs);
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &exitCode);
    else if (wait == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, 1);
    std::string stderrText;
    std::array<char, 1024> stderrBuffer{};
    DWORD stderrLength = 0;
    while (ReadFile(stderrRead, stderrBuffer.data(), static_cast<DWORD>(stderrBuffer.size()), &stderrLength, nullptr) && stderrLength)
        stderrText.append(stderrBuffer.data(), stderrLength);
    CloseHandle(stderrRead);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exitCode != 0) {
        error = wait == WAIT_TIMEOUT
            ? "The independent SSH archive publish command timed out."
            : stderrText.empty()
                ? "The independent SSH archive publish command failed (exit status " + std::to_string(exitCode) + ")."
                : stderrText;
        return false;
    }
    return true;
}

bool StreamPipeToWindowsArchiveFile(HANDLE pipe, RemoteEndpoint& target, std::string& error)
{
    if (!target.cs || !target.cs->sftpsession) {
        error = "The Windows SFTP archive target is unavailable.";
        return false;
    }

    // Keep the staging upload and the publish command isolated from panel I/O
    // on the same non-blocking libssh2 session.
    ScopedSshSessionUse archiveUse(target.cs);
    const std::string parent = RemoteParentPath(target.remotePath);
    const std::string temporaryPath = parent + "/.sftp-archive-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64()) + ".tmp";
    std::unique_ptr<ISftpHandle> output;
    if (!OpenWindowsArchiveTemp(target, temporaryPath, output)) {
        error = "Could not create a temporary archive on the Windows SFTP target.";
        return false;
    }

    SendResult(pipe, 0, "");
    std::array<char, kChunkSize> buffer{};
    bool ok = true;
    bool drainSender = false;
    for (;;) {
        DWORD length = 0;
        if (!ReadExact(pipe, &length, sizeof(length))) {
            error = "Archive sender disconnected.";
            ok = false;
            break;
        }
        if (length == 0)
            break;
        if (length > buffer.size() || !ReadExact(pipe, buffer.data(), length)) {
            error = "Archive sender disconnected.";
            ok = false;
            break;
        }
        if (!WriteWindowsArchiveTemp(target, output.get(), buffer.data(), length, error)) {
            ok = false;
            drainSender = true;
            break;
        }
        if (!ok)
            break;
    }
    if (drainSender)
        DrainArchiveSender(pipe);

    if (!CloseWindowsArchiveTemp(target, output)) {
        error = "Could not finish the temporary Windows archive through SFTP.";
        ok = false;
    }

    if (ok)
        ok = PublishWindowsArchiveTemp(target, temporaryPath, error);
    if (!ok)
        RemoveWindowsArchiveTemp(target, temporaryPath);
    return ok;
}

bool StreamRemoteToWindowsArchive(RemoteEndpoint& source, const std::string& sourceCommand,
                                  RemoteEndpoint& target, std::string& error)
{
    if (!target.cs || !target.cs->sftpsession) {
        error = "The Windows SFTP archive target is unavailable.";
        return false;
    }

    ScopedSshSessionUse targetUse(target.cs);
    ScopedSshSessionUse sourceUse(source.cs);
    const std::string parent = RemoteParentPath(target.remotePath);
    const std::string temporaryPath = parent + "/.sftp-archive-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64()) + ".tmp";
    std::unique_ptr<ISftpHandle> output;
    if (!OpenWindowsArchiveTemp(target, temporaryPath, output)) {
        error = "Could not create a temporary archive on the Windows SFTP target.";
        return false;
    }

    auto sourceChannel = StartCommand(source.cs, sourceCommand, error);
    if (!sourceChannel) {
        CloseWindowsArchiveTemp(target, output);
        RemoveWindowsArchiveTemp(target, temporaryPath);
        return false;
    }

    std::array<char, kChunkSize> buffer{};
    bool ok = true;
    for (;;) {
        ssize_t count = 0;
        if (!ReadRemoteChunk(sourceChannel.get(), source.cs, buffer.data(), buffer.size(), count, error)) {
            ok = false;
            break;
        }
        if (count == 0)
            break;
        // Keep draining a failed producer so its remote tar cannot block on stdout.
        if (ok && !WriteWindowsArchiveTemp(target, output.get(), buffer.data(), static_cast<size_t>(count), error))
            ok = false;
    }
    const bool sourceOk = CloseCommand(sourceChannel, source.cs, false, error);
    if (!CloseWindowsArchiveTemp(target, output)) {
        if (error.empty())
            error = "Could not finish the temporary Windows archive through SFTP.";
        ok = false;
    }
    if (ok && sourceOk)
        ok = PublishWindowsArchiveTemp(target, temporaryPath, error);
    if (!ok || !sourceOk)
        RemoveWindowsArchiveTemp(target, temporaryPath);
    return ok && sourceOk;
}

bool StreamRemoteToRemote(RemoteEndpoint& source, const std::string& sourceCommand,
                          RemoteEndpoint& target, const std::string& targetCommand, std::string& error)
{
    ScopedSshSessionUse targetUse(target.cs);
    ScopedSshSessionUse sourceUse(source.cs);
    auto targetChannel = StartCommand(target.cs, targetCommand, error);
    if (!targetChannel)
        return false;
    auto sourceChannel = StartCommand(source.cs, sourceCommand, error);
    if (!sourceChannel) {
        CloseCommand(targetChannel, target.cs, true, error);
        return false;
    }
    std::array<char, kChunkSize> buffer{};
    bool ok = true;
    for (;;) {
        ssize_t count = 0;
        if (!ReadRemoteChunk(sourceChannel.get(), source.cs, buffer.data(), buffer.size(), count, error)) {
            ok = false;
            break;
        }
        if (count == 0)
            break;
        if (!WriteRemoteAll(targetChannel.get(), target.cs, buffer.data(), static_cast<size_t>(count), error)) {
            ok = false;
            break;
        }
    }
    const bool sourceOk = CloseCommand(sourceChannel, source.cs, false, error);
    const bool targetOk = CloseCommand(targetChannel, target.cs, true, error);
    return ok && sourceOk && targetOk;
}

bool ReadRequest(HANDLE pipe, ArchiveRequest& request, std::string& sourcePath,
                 std::string& targetPath, std::string& items)
{
    if (!ReadExact(pipe, &request, sizeof(request)) || request.magic != kArchiveMagic ||
        request.sourcePathBytes > kMaxPathBytes || request.targetPathBytes > kMaxPathBytes ||
        request.itemBytes > kMaxItemBytes)
        return false;
    return ReadText(pipe, request.sourcePathBytes, sourcePath) &&
           ReadText(pipe, request.targetPathBytes, targetPath) &&
           ReadText(pipe, request.itemBytes, items);
}

void ServeClient(HANDLE pipe)
{
    ArchiveRequest request{};
    std::string sourcePath;
    std::string targetPath;
    std::string items;
    if (!ReadRequest(pipe, request, sourcePath, targetPath, items)) {
        SendResult(pipe, 1, "Invalid archive request.");
        return;
    }

    std::string error;
    bool ok = false;
    if (request.operation == kArchivePut || request.operation == kArchiveExtract) {
        RemoteEndpoint target;
        if (OpenEndpoint(targetPath, target, error)) {
            if (request.operation == kArchivePut && IsWindowsRemotePath(target.remotePath)) {
                ok = StreamPipeToWindowsArchiveFile(pipe, target, error);
            } else {
                const std::string command = BuildRemoteReceiveCommand(target, request.operation == kArchiveExtract);
                ok = StreamPipeToConsumer(pipe, target, command, error);
            }
            if (ok)
                InvalidateSftpManifestCache(target.cs);
        }
    } else if (request.operation == kArchiveGet || request.operation == kArchivePack) {
        RemoteEndpoint source;
        if (OpenEndpoint(sourcePath, source, error)) {
            const std::string command = request.operation == kArchiveGet
                ? BuildRemoteReadFileCommand(source)
                : BuildRemotePackCommand(source, items);
            ok = StreamProducerToPipe(pipe, source, command, error);
        }
    } else if (request.operation == kArchivePackToRemote || request.operation == kArchivePackExtractRemote) {
        RemoteEndpoint source;
        RemoteEndpoint target;
        if (OpenEndpoint(sourcePath, source, error) && OpenEndpoint(targetPath, target, error)) {
            const std::string sourceCommand = BuildRemotePackCommand(source, items);
            if (request.operation == kArchivePackToRemote && IsWindowsRemotePath(target.remotePath)) {
                ok = StreamRemoteToWindowsArchive(source, sourceCommand, target, error);
            } else {
                const std::string targetCommand = BuildRemoteReceiveCommand(target, request.operation == kArchivePackExtractRemote);
                ok = StreamRemoteToRemote(source, sourceCommand, target, targetCommand, error);
            }
            if (ok)
                InvalidateSftpManifestCache(target.cs);
        }
    } else if (request.operation == kArchiveIsDirectory) {
        RemoteEndpoint source;
        if (OpenEndpoint(sourcePath, source, error) && source.cs->sftpsession) {
            LIBSSH2_SFTP_ATTRIBUTES attrs{};
            int rc = 0;
            do {
                ScopedSshSessionUse sessionUse(source.cs);
                rc = source.cs->sftpsession->stat(source.remotePath.c_str(), &attrs);
                if (rc == LIBSSH2_ERROR_EAGAIN)
                    WaitForSshIo(source.cs, DIRECTORY_IO_POLL_MS);
            } while (rc == LIBSSH2_ERROR_EAGAIN);
            ok = rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                 LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            if (!ok && error.empty())
                error = "The selected remote item is not a directory.";
        }
    } else if (request.operation == kArchiveDeleteRemote) {
        RemoteEndpoint source;
        if (OpenEndpoint(sourcePath, source, error) && !items.empty()) {
            auto channel = StartCommand(source.cs, BuildRemoteDeleteCommand(source, items), error);
            ok = channel && CloseCommand(channel, source.cs, false, error);
            if (ok)
                InvalidateSftpManifestCache(source.cs);
        }
    } else if (request.operation == kArchivePrewarmManifest) {
        ok = PrewarmManifest(sourcePath, error);
    } else if (request.operation == kArchiveLogStatus) {
        std::string sessionName;
        std::wstring relativePath;
        if (ParseSftpPath(sourcePath, sessionName, relativePath) && LogProc) {
            // The virtual path is the source of truth; DisplayName can belong
            // to another active connection after concurrent panel activity.
            const std::string status = "[" + sessionName + "] " + items;
            LogProc(PluginNumber, MSGTYPE_DETAILS, status.c_str());
            ok = true;
        } else if (error.empty()) {
            error = "Invalid SFTP path or unavailable log callback.";
        }
    } else if (request.operation == kArchiveShowError) {
        if (RequestProcW) {
            const std::wstring message = unicode_util::utf8_to_wstring(items);
            RequestProcW(PluginNumber, RT_MsgOK, L"SFTP Archive Router", message.c_str(), nullptr, 0);
            ok = true;
        } else {
            error = "Total Commander cannot display plugin error messages.";
        }
    } else if (request.operation == kArchiveTunnelStatus) {
        RemoteEndpoint endpoint;
        if (OpenEndpoint(sourcePath, endpoint, error)) {
            ok = true;
            if (ok) {
                for (size_t index = 0; index < endpoint.cs->sshTunnels.size(); ++index) {
                    error += endpoint.cs->sshTunnels[index].startOnConnect ? "1\t" : "0\t";
                    error += FormatSshTunnelRule(endpoint.cs->sshTunnels[index]);
                    error += '\n';
                }
            }
        }
    } else if (request.operation == kArchiveTunnelSetEnabled) {
        RemoteEndpoint endpoint;
        const size_t separator = items.find('|');
        const std::string indexText = items.substr(0, separator);
        const bool enabled = separator != std::string::npos && items.substr(separator + 1) == "1";
        const unsigned long index = strtoul(indexText.c_str(), nullptr, 10);
        if (!OpenEndpoint(sourcePath, endpoint, error)) {
            // OpenEndpoint populated a useful active-session error.
        } else if (separator == std::string::npos || index >= endpoint.cs->sshTunnels.size()) {
            error = "Invalid SSH tunnel selection.";
        } else {
            if (!endpoint.cs->sshTunnelManager)
                endpoint.cs->sshTunnelManager = std::make_unique<SshTunnelManager>(endpoint.cs);
            ok = endpoint.cs->sshTunnelManager->SetEnabled(index, enabled, error);
            if (ok) {
                endpoint.cs->sshTunnels[index].startOnConnect = enabled;
                ok = SaveSshTunnelRules(endpoint.cs, error);
            }
        }
    } else if (request.operation == kArchiveTunnelReplaceRules) {
        RemoteEndpoint endpoint;
        std::vector<SshTunnelRule> rules;
        if (!OpenEndpoint(sourcePath, endpoint, error)) {
            // OpenEndpoint populated a useful active-session error.
        } else if (!ParseTunnelRuleLines(items, rules, error)) {
            // ParseTunnelRuleLines populated a useful validation error.
        } else {
            std::vector<SshTunnelRule> oldRules = endpoint.cs->sshTunnels;
            if (endpoint.cs->sshTunnelManager)
                endpoint.cs->sshTunnelManager.reset();
            endpoint.cs->sshTunnels = std::move(rules);
            ok = true;
            if (!endpoint.cs->sshTunnels.empty()) {
                endpoint.cs->sshTunnelManager = std::make_unique<SshTunnelManager>(endpoint.cs);
                std::string tunnelError;
                if (!endpoint.cs->sshTunnelManager->StartDefaults(tunnelError) && !tunnelError.empty()) {
                    error = tunnelError;
                    ok = false;
                }
            }
            if (!ok) {
                endpoint.cs->sshTunnelManager.reset();
                endpoint.cs->sshTunnels = std::move(oldRules);
                if (!endpoint.cs->sshTunnels.empty()) {
                    endpoint.cs->sshTunnelManager = std::make_unique<SshTunnelManager>(endpoint.cs);
                    std::string ignored;
                    endpoint.cs->sshTunnelManager->StartDefaults(ignored);
                }
            }
            if (ok && !SaveSshTunnelRules(endpoint.cs, error)) {
                endpoint.cs->sshTunnelManager.reset();
                endpoint.cs->sshTunnels = std::move(oldRules);
                if (!endpoint.cs->sshTunnels.empty()) {
                    endpoint.cs->sshTunnelManager = std::make_unique<SshTunnelManager>(endpoint.cs);
                    std::string ignored;
                    endpoint.cs->sshTunnelManager->StartDefaults(ignored);
                }
                ok = false;
            }
        }
    } else {
        error = "Unsupported archive operation.";
    }
    SFTP_LOG("ARCHIVE", "operation=%lu result=%d error='%s'", request.operation, ok ? 0 : 1, error.c_str());
    SendResult(pipe, ok ? 0 : 1, error);
    // DisconnectNamedPipe discards queued data. Ensure the router has consumed
    // the final result before releasing this pipe instance.
    FlushFileBuffers(pipe);
}

void ListenerMain()
{
    const std::wstring pipeName = L"\\\\.\\pipe\\SftpArchive." + std::to_wstring(GetCurrentProcessId());
    while (g_running.load()) {
        HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, kChunkSize, kChunkSize, 1000, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            return;
        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected)
            ServeClient(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

} // namespace

void StartSftpArchivePipeService() noexcept
{
    if (g_running.exchange(true))
        return;
    g_listener = std::thread(ListenerMain);
}

void StopSftpArchivePipeService() noexcept
{
    if (!g_running.exchange(false))
        return;
    const std::wstring pipeName = L"\\\\.\\pipe\\SftpArchive." + std::to_wstring(GetCurrentProcessId());
    HANDLE wake = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE)
        CloseHandle(wake);
    if (g_listener.joinable())
        g_listener.join();
}

bool TryGetSftpManifestDirectoryListing(pConnectSettings cs, LPCWSTR remoteDir,
                                        std::vector<WIN32_FIND_DATAW>& entries) noexcept
{
    if (!cs || !remoteDir)
        return false;
    const std::string path = ToRemotePathA(cs, remoteDir);
    // The synthetic root listing includes Total Commander's home shortcut.
    if (path.size() <= 1)
        return false;
    std::lock_guard<std::mutex> lock(g_manifestMutex);
    if (!g_manifestCache || g_manifestCache->cs != cs ||
        GetTickCount64() - g_manifestCache->createdAt > kManifestLifetimeMs) {
        g_manifestCache.reset();
        return false;
    }
    const auto found = g_manifestCache->directories.find(path);
    if (found == g_manifestCache->directories.end())
        return false;
    entries = found->second;
    return true;
}

void InvalidateSftpManifestCache(pConnectSettings cs) noexcept
{
    std::lock_guard<std::mutex> lock(g_manifestMutex);
    if (!cs || (g_manifestCache && g_manifestCache->cs == cs))
        g_manifestCache.reset();
}
