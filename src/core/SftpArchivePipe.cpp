#include "global.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "CoreUtils.h"
#include "SftpArchivePipe.h"
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
constexpr DWORD kMaxPathBytes = 64 * 1024;
constexpr DWORD kMaxItemBytes = 1024 * 1024;
constexpr DWORD kChunkSize = 64 * 1024;
constexpr size_t kMaxManifestBytes = 64 * 1024 * 1024;
constexpr ULONGLONG kManifestLifetimeMs = 10 * 60 * 1000;

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
    endpoint.remotePath = ToRemotePathA(endpoint.cs, relativePath.c_str());
    while (endpoint.remotePath.size() > 1 && endpoint.remotePath.ends_with("/."))
        endpoint.remotePath.resize(endpoint.remotePath.size() - 2);
    while (endpoint.remotePath.size() > 1 && endpoint.remotePath.ends_with('/'))
        endpoint.remotePath.pop_back();
    return !endpoint.remotePath.empty();
}

bool WriteRemoteAll(ISshChannel* channel, pConnectSettings cs, const char* data, size_t length)
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
            if (!WaitForSshIo(cs, DIRECTORY_IO_POLL_MS))
                return false;
            continue;
        }
        return false;
    }
    return true;
}

std::unique_ptr<ISshChannel> StartCommand(pConnectSettings cs, const std::string& command, std::string& error)
{
    std::unique_ptr<ISshChannel> channel;
    {
        ScopedSshSessionUse sessionUse(cs);
        channel = ConnectChannel(cs->session.get(), cs->sock);
    }
    if (!channel || WaitForOperation([&] {
            ScopedSshSessionUse sessionUse(cs);
            return channel->exec(command.c_str());
        }, SSH_AUTH_STAGE_TIMEOUT_MS, cs) < 0) {
        error = "Could not start the remote archive command.";
        return {};
    }
    return channel;
}

bool CloseCommand(std::unique_ptr<ISshChannel>& channel, pConnectSettings cs, bool sendEof,
                  std::string& error)
{
    if (!channel)
        return false;
    if (sendEof && WaitForOperation([&] {
            ScopedSshSessionUse sessionUse(cs);
            return channel->sendEof();
        }, SSH_AUTH_STAGE_TIMEOUT_MS, cs) < 0) {
        error = "Could not finish the remote archive command.";
        return false;
    }
    if (WaitForOperation([&] {
            ScopedSshSessionUse sessionUse(cs);
            return channel->waitEof();
        }, SSH_AUTH_STAGE_TIMEOUT_MS, cs) < 0) {
        error = "The remote archive command did not finish.";
        return false;
    }

    std::array<char, 2048> stderrData{};
    for (;;) {
        ssize_t rc = 0;
        {
            ScopedSshSessionUse sessionUse(cs);
            rc = channel->readStderr(stderrData.data(), stderrData.size());
        }
        if (rc > 0) {
            error.append(stderrData.data(), static_cast<size_t>(rc));
            continue;
        }
        break;
    }
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
    if (exitStatus != 0) {
        if (error.empty())
            error = "The remote archive command failed.";
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
        if (!WaitForSshIo(cs, DIRECTORY_IO_POLL_MS)) {
            error = "The remote archive stream timed out.";
            return false;
        }
    }
}

std::string Quote(const std::string& text)
{
    return "'" + string_util::ShellQuoteSingle(text) + "'";
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
    const std::string command = "find " + Quote(endpoint.remotePath) +
        " -printf '%y\\000%s\\000%T@\\000%P\\000'";
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
    const DWORD endMarker = 0;
    if (ok && !WriteExact(pipe, &endMarker, sizeof(endMarker))) {
        error = "Archive receiver disconnected.";
        ok = false;
    }
    return CloseCommand(channel, source.cs, false, error) && ok;
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
    for (;;) {
        DWORD length = 0;
        if (!ReadExact(pipe, &length, sizeof(length))) {
            error = "Archive sender disconnected.";
            ok = false;
            break;
        }
        if (length == 0)
            break;
        if (length > buffer.size() || !ReadExact(pipe, buffer.data(), length) ||
            !WriteRemoteAll(channel.get(), target.cs, buffer.data(), length)) {
            error = "Could not stream archive data to the remote server.";
            ok = false;
            break;
        }
    }
    return CloseCommand(channel, target.cs, true, error) && ok;
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
        if (!WriteRemoteAll(targetChannel.get(), target.cs, buffer.data(), static_cast<size_t>(count))) {
            error = "Could not stream archive data to the target server.";
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
            const std::string command = request.operation == kArchivePut
                ? "cat > " + Quote(target.remotePath)
                : "tar -xf - -C " + Quote(target.remotePath);
            ok = StreamPipeToConsumer(pipe, target, command, error);
            if (ok)
                InvalidateSftpManifestCache(target.cs);
        }
    } else if (request.operation == kArchiveGet || request.operation == kArchivePack) {
        RemoteEndpoint source;
        if (OpenEndpoint(sourcePath, source, error)) {
            const std::string command = request.operation == kArchiveGet
                ? "cat " + Quote(source.remotePath)
                : BuildRemotePackCommand(source, items);
            ok = StreamProducerToPipe(pipe, source, command, error);
        }
    } else if (request.operation == kArchivePackToRemote || request.operation == kArchivePackExtractRemote) {
        RemoteEndpoint source;
        RemoteEndpoint target;
        if (OpenEndpoint(sourcePath, source, error) && OpenEndpoint(targetPath, target, error)) {
            const std::string sourceCommand = BuildRemotePackCommand(source, items);
            const std::string targetCommand = request.operation == kArchivePackToRemote
                ? "cat > " + Quote(target.remotePath)
                : "tar -xf - -C " + Quote(target.remotePath);
            ok = StreamRemoteToRemote(source, sourceCommand, target, targetCommand, error);
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
