#include "global.h"
#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <climits>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "SftpClient.h"
#include "SftpInternal.h"
#include "PluginEntryPoints.h"
#include "SshTunnelManager.h"
#include "TunnelLog.h"

namespace {

constexpr int kTunnelBacklog = 16;
constexpr DWORD kTunnelPollMs = 100;
constexpr DWORD kSocksHandshakeTimeoutMs = 10000;
constexpr DWORD kDirectTcpipTimeoutMs = 30000;
constexpr DWORD kRemoteForwardRetryMs = 3000;
constexpr DWORD kTunnelStopTimeoutMs = 3000;

void LogTunnelMessage(int messageType, const std::string& message)
{
    DispatchTunnelLog(PluginNumber, messageType, message, LogProc, LogProcW);
}

std::string TunnelDescription(const SshTunnelRule& rule)
{
    const std::string formatted = FormatSshTunnelRule(rule);
    return formatted.size() > 2 ? formatted.substr(2) : formatted;
}

void LogTunnelStartFailed(const tConnectSettings* session, const std::string& error)
{
    const std::string message = "SFTP tunnel startup failed for '" +
        (session ? session->DisplayName : std::string("(unknown)")) + "': " + error;
    LogTunnelMessage(MSGTYPE_IMPORTANTERROR, message);
}

void LogTunnelStarted(const tConnectSettings* session, const SshTunnelRule& rule)
{
    const std::string message = "SFTP tunnel started for '" +
        (session ? session->DisplayName : std::string("(unknown)")) + "': " + TunnelDescription(rule);
    LogTunnelMessage(MSGTYPE_DETAILS, message);
}

void LogTunnelStopped(const tConnectSettings* session, const SshTunnelRule& rule)
{
    const std::string message = "SFTP tunnel stopped for '" +
        (session ? session->DisplayName : std::string("(unknown)")) + "': " + TunnelDescription(rule);
    LogTunnelMessage(MSGTYPE_DETAILS, message);
}

bool SameTunnelEndpoint(const SshTunnelRule& left, const SshTunnelRule& right)
{
    return left.type == right.type && _stricmp(left.bindAddress.c_str(), right.bindAddress.c_str()) == 0 &&
        left.listenPort == right.listenPort && _stricmp(left.targetHost.c_str(), right.targetHost.c_str()) == 0 &&
        left.targetPort == right.targetPort;
}

bool SameTunnelRule(const SshTunnelRule& left, const SshTunnelRule& right)
{
    return left.startOnConnect == right.startOnConnect && SameTunnelEndpoint(left, right);
}

void CloseSocket(SOCKET& socket) noexcept
{
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
        socket = INVALID_SOCKET;
    }
}

bool WaitSocket(SOCKET socket, bool write, DWORD timeout)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval tv{ static_cast<long>(timeout / 1000), static_cast<long>((timeout % 1000) * 1000) };
    const int result = select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
    return result > 0;
}

SOCKET ConnectTcp(const std::string& host, unsigned short port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results) != 0)
        return INVALID_SOCKET;
    SOCKET connected = INVALID_SOCKET;
    for (addrinfo* entry = results; entry; entry = entry->ai_next) {
        SOCKET socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (socket == INVALID_SOCKET)
            continue;
        if (connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
            connected = socket;
            break;
        }
        closesocket(socket);
    }
    freeaddrinfo(results);
    return connected;
}

SOCKET CreateListener(const std::string& bindAddress, unsigned short port, std::string& error)
{
    addrinfo hints{};
    hints.ai_family = bindAddress.find(':') == std::string::npos ? AF_INET : AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(bindAddress.c_str(), service.c_str(), &hints, &results) != 0) {
        error = "The local tunnel bind address is invalid: " + bindAddress;
        return INVALID_SOCKET;
    }
    SOCKET listener = INVALID_SOCKET;
    for (addrinfo* entry = results; entry; entry = entry->ai_next) {
        listener = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (listener == INVALID_SOCKET)
            continue;
        const BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        if (bind(listener, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0 &&
            listen(listener, kTunnelBacklog) == 0)
            break;
        CloseSocket(listener);
    }
    freeaddrinfo(results);
    if (listener == INVALID_SOCKET)
        error = "Could not listen on " + bindAddress + ":" + service + " (Windows error " + std::to_string(WSAGetLastError()) + ").";
    return listener;
}

bool ReceiveExact(SOCKET socket, char* data, size_t length)
{
    while (length) {
        const int got = recv(socket, data, static_cast<int>((std::min)(length, static_cast<size_t>(INT_MAX))), 0);
        if (got <= 0)
            return false;
        data += got;
        length -= static_cast<size_t>(got);
    }
    return true;
}

bool SendAll(SOCKET socket, const char* data, size_t length)
{
    while (length) {
        const int sent = send(socket, data, static_cast<int>((std::min)(length, static_cast<size_t>(INT_MAX))), 0);
        if (sent <= 0)
            return false;
        data += sent;
        length -= static_cast<size_t>(sent);
    }
    return true;
}

bool SocksConnect(SOCKET socket, std::string& host, unsigned short& port)
{
    std::array<unsigned char, 2> hello{};
    if (!ReceiveExact(socket, reinterpret_cast<char*>(hello.data()), hello.size()) || hello[0] != 5)
        return false;
    std::vector<unsigned char> methods(hello[1]);
    if (!ReceiveExact(socket, reinterpret_cast<char*>(methods.data()), methods.size()))
        return false;
    const unsigned char accepted[] = { 5, 0 };
    if (std::find(methods.begin(), methods.end(), 0) == methods.end() ||
        !SendAll(socket, reinterpret_cast<const char*>(accepted), sizeof(accepted)))
        return false;
    std::array<unsigned char, 4> request{};
    if (!ReceiveExact(socket, reinterpret_cast<char*>(request.data()), request.size()) ||
        request[0] != 5 || request[1] != 1)
        return false;
    if (request[3] == 1) {
        std::array<unsigned char, 4> address{};
        if (!ReceiveExact(socket, reinterpret_cast<char*>(address.data()), address.size()))
            return false;
        char text[INET_ADDRSTRLEN]{};
        if (!InetNtopA(AF_INET, address.data(), text, sizeof(text)))
            return false;
        host = text;
    } else if (request[3] == 4) {
        std::array<unsigned char, 16> address{};
        if (!ReceiveExact(socket, reinterpret_cast<char*>(address.data()), address.size()))
            return false;
        char text[INET6_ADDRSTRLEN]{};
        if (!InetNtopA(AF_INET6, address.data(), text, sizeof(text)))
            return false;
        host = text;
    } else if (request[3] == 3) {
        unsigned char length = 0;
        if (!ReceiveExact(socket, reinterpret_cast<char*>(&length), 1) || length == 0)
            return false;
        host.resize(length);
        if (!ReceiveExact(socket, host.data(), length))
            return false;
    } else {
        return false;
    }
    std::array<unsigned char, 2> portBytes{};
    if (!ReceiveExact(socket, reinterpret_cast<char*>(portBytes.data()), portBytes.size()))
        return false;
    port = static_cast<unsigned short>((portBytes[0] << 8) | portBytes[1]);
    return port != 0;
}

bool SendSocksReply(SOCKET socket, unsigned char code)
{
    const unsigned char reply[] = { 5, code, 0, 1, 0, 0, 0, 0, 0, 0 };
    return SendAll(socket, reinterpret_cast<const char*>(reply), sizeof(reply));
}

void SetTunnelSocketTimeouts(SOCKET socket)
{
    const DWORD timeout = kTunnelPollMs;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

void SetSocksHandshakeTimeouts(SOCKET socket)
{
    const DWORD timeout = kSocksHandshakeTimeoutMs;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

void FreeRelayChannel(pConnectSettings session, std::unique_ptr<ISshChannel>& channel) noexcept
{
    if (!channel)
        return;

    // channelFree() owns the EOF/close sequence for a direct-tcpip channel.
    // Drain that single state machine while holding the session lease instead
    // of advancing it separately through sendEof/channelClose first.
    ScopedSshSessionUse use(session);
    WaitForOperation([&] { return channel->channelFree(); }, 2000, session);
    channel.reset();
}

void Relay(pConnectSettings session, SOCKET local, std::unique_ptr<ISshChannel> channel, std::atomic<bool>& stop,
           std::mutex& socketMutex, std::vector<SOCKET>& sockets)
{
    std::array<char, 64 * 1024> buffer{};
    while (!stop.load(std::memory_order_acquire)) {
        if (WaitSocket(local, false, kTunnelPollMs)) {
            const int received = recv(local, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0)
                break;
            size_t sent = 0;
            while (sent < static_cast<size_t>(received) && !stop.load(std::memory_order_acquire)) {
                ssize_t rc = 0;
                {
                    ScopedSshSessionUse use(session);
                    rc = channel->write(buffer.data() + sent, static_cast<size_t>(received) - sent);
                    if (rc == LIBSSH2_ERROR_EAGAIN)
                        WaitForSshIo(session, kTunnelPollMs);
                }
                if (rc < 0 && rc != LIBSSH2_ERROR_EAGAIN)
                    goto done;
                if (rc > 0)
                    sent += static_cast<size_t>(rc);
            }
        }
        for (;;) {
            ssize_t rc = 0;
            {
                ScopedSshSessionUse use(session);
                rc = channel->read(buffer.data(), buffer.size());
            }
            if (rc == LIBSSH2_ERROR_EAGAIN) {
                WaitForSshIo(session, kTunnelPollMs);
                break;
            }
            if (rc <= 0)
                goto done;
            if (!SendAll(local, buffer.data(), static_cast<size_t>(rc)))
                goto done;
        }
    }
done:
    {
        std::lock_guard<std::mutex> lock(socketMutex);
        const auto found = std::find(sockets.begin(), sockets.end(), local);
        if (found != sockets.end())
            sockets.erase(found);
    }
    CloseSocket(local);
    FreeRelayChannel(session, channel);
}

std::unique_ptr<ISshChannel> OpenDirectTcpip(pConnectSettings session, const std::string& host,
                                              unsigned short port)
{
    // libssh2 retains incomplete direct-tcpip state on the session after
    // EAGAIN. Keep the lease until that same request is resumed or abandoned.
    ScopedSshSessionUse use(session);
    const SYSTICKS start = get_sys_ticks();
    for (;;) {
        std::unique_ptr<ISshChannel> channel = session->session->directTcpip(host.c_str(), port, "127.0.0.1", 0);
        const int error = channel ? 0 : session->session->lastErrno();
        if (channel || error != LIBSSH2_ERROR_EAGAIN)
            return channel;
        if (get_ticks_between(start) >= kDirectTcpipTimeoutMs)
            return {};
        if (!WaitForSshIo(session, kTunnelPollMs))
            continue;
    }
}

std::unique_ptr<ISshForwardListener> OpenForwardListener(pConnectSettings session, const SshTunnelRule& rule,
                                                           int& boundPort)
{
    const SYSTICKS start = get_sys_ticks();
    for (;;) {
        std::unique_ptr<ISshForwardListener> listener;
        int error = 0;
        {
            ScopedSshSessionUse use(session);
            listener = session->session->forwardListen(rule.bindAddress.c_str(), rule.listenPort,
                                                        &boundPort, kTunnelBacklog);
            error = listener ? 0 : session->session->lastErrno();
        }
        if (listener)
            return listener;
        if (error == LIBSSH2_ERROR_EAGAIN) {
            if (!WaitForSshIo(session, kTunnelPollMs))
                return {};
            continue;
        }
        if (get_ticks_between(start) >= kRemoteForwardRetryMs)
            return {};
        Sleep(kTunnelPollMs);
    }
}

}

struct SshTunnelManager::Tunnel {
    explicit Tunnel(SshTunnelRule tunnelRule) : rule(std::move(tunnelRule)) {}
    SshTunnelRule rule;
    SOCKET localListener = INVALID_SOCKET;
    std::unique_ptr<ISshForwardListener> remoteListener;
    std::thread listenerThread;
    std::vector<std::thread> relayThreads;
    std::vector<SOCKET> relaySockets;
    std::atomic<bool> stop{ false };
    std::atomic<SshTunnelRuntimeState> runtime{ SshTunnelRuntimeState::stopped };
    std::string error;
    std::mutex mutex;
};

SshTunnelManager::SshTunnelManager(tConnectSettings* session) : session_(session)
{
    if (session_) {
        for (const SshTunnelRule& rule : session_->sshTunnels)
            tunnels_.push_back(std::make_unique<Tunnel>(rule));
    }
}

SshTunnelManager::~SshTunnelManager()
{
    StopAll();
}

bool SshTunnelManager::StartDefaults(std::string& error)
{
    std::lock_guard<std::mutex> managerLock(mutex_);
    bool allStarted = true;
    for (size_t index = 0; index < tunnels_.size(); ++index) {
        if (!tunnels_[index]->rule.startOnConnect)
            continue;
        std::string ruleError;
        if (!StartTunnel(*tunnels_[index], ruleError)) {
            allStarted = false;
            if (error.empty())
                error = std::move(ruleError);
        }
    }
    return allStarted;
}

bool SshTunnelManager::SetEnabled(size_t index, bool enabled, std::string& error)
{
    std::lock_guard<std::mutex> managerLock(mutex_);
    if (!session_ || index >= tunnels_.size()) {
        error = "Tunnel rule is unavailable.";
        LogTunnelStartFailed(session_, error);
        return false;
    }
    Tunnel& tunnel = *tunnels_[index];
    tunnel.rule.startOnConnect = enabled;
    return enabled ? StartTunnel(tunnel, error) : StopTunnel(tunnel, error);
}

bool SshTunnelManager::StopTunnel(Tunnel& tunnel, std::string& error) noexcept
{
    const bool wasRunning = tunnel.runtime.load(std::memory_order_acquire) == SshTunnelRuntimeState::running;
    tunnel.stop.store(true, std::memory_order_release);
    CloseSocket(tunnel.localListener);
    {
        std::lock_guard<std::mutex> lock(tunnel.mutex);
        for (SOCKET socket : tunnel.relaySockets)
            shutdown(socket, SD_BOTH);
    }
    if (tunnel.remoteListener) {
        ScopedSshSessionUse use(session_);
        const SYSTICKS start = get_sys_ticks();
        while (tunnel.remoteListener->cancel() == LIBSSH2_ERROR_EAGAIN) {
            if (get_ticks_between(start) >= kTunnelStopTimeoutMs) {
                error = "Timed out while stopping remote tunnel " + TunnelDescription(tunnel.rule) + ".";
                break;
            }
            WaitForSshIo(session_, kTunnelPollMs);
        }
    }
    if (tunnel.listenerThread.joinable())
        tunnel.listenerThread.join();
    tunnel.remoteListener.reset();
    for (std::thread& worker : tunnel.relayThreads)
        if (worker.joinable()) worker.join();
    tunnel.relayThreads.clear();
    tunnel.runtime.store(SshTunnelRuntimeState::stopped, std::memory_order_release);
    tunnel.error = error;
    if (wasRunning)
        LogTunnelStopped(session_, tunnel.rule);
    return error.empty();
}

bool SshTunnelManager::StartTunnel(Tunnel& tunnel, std::string& error)
{
    if (tunnel.runtime.load(std::memory_order_acquire) == SshTunnelRuntimeState::running)
        return true;
    tunnel.error.clear();
    tunnel.stop.store(false, std::memory_order_release);
    if (tunnel.rule.type == SshTunnelType::remote) {
        int boundPort = tunnel.rule.listenPort;
        tunnel.remoteListener = OpenForwardListener(session_, tunnel.rule, boundPort);
        if (!tunnel.remoteListener) {
            error = "SSH server refused remote tunnel " + TunnelDescription(tunnel.rule) +
                ". The remote listen port may still be releasing, already in use, or blocked by the server's remote forwarding policy.";
            tunnel.error = error;
            tunnel.runtime.store(SshTunnelRuntimeState::failed, std::memory_order_release);
            LogTunnelStartFailed(session_, error);
            return false;
        }
        tunnel.listenerThread = std::thread([this, &tunnel] {
            while (!tunnel.stop.load(std::memory_order_acquire)) {
                std::unique_ptr<ISshChannel> channel;
                {
                    ScopedSshSessionUse use(session_);
                    channel = tunnel.remoteListener->accept();
                }
                if (!channel) {
                    Sleep(kTunnelPollMs);
                    continue;
                }
                SOCKET local = ConnectTcp(tunnel.rule.targetHost, tunnel.rule.targetPort);
                if (local == INVALID_SOCKET)
                    continue;
                SetTunnelSocketTimeouts(local);
                {
                    std::lock_guard<std::mutex> lock(tunnel.mutex);
                    tunnel.relaySockets.push_back(local);
                }
                tunnel.relayThreads.emplace_back(Relay, session_, local, std::move(channel), std::ref(tunnel.stop),
                                                 std::ref(tunnel.mutex), std::ref(tunnel.relaySockets));
            }
        });
    } else {
        tunnel.localListener = CreateListener(tunnel.rule.bindAddress, tunnel.rule.listenPort, error);
        if (tunnel.localListener == INVALID_SOCKET) {
            tunnel.error = error;
            tunnel.runtime.store(SshTunnelRuntimeState::failed, std::memory_order_release);
            LogTunnelStartFailed(session_, error);
            return false;
        }
        tunnel.listenerThread = std::thread([this, &tunnel] {
            while (!tunnel.stop.load(std::memory_order_acquire)) {
                if (!WaitSocket(tunnel.localListener, false, kTunnelPollMs))
                    continue;
                SOCKET local = accept(tunnel.localListener, nullptr, nullptr);
                if (local == INVALID_SOCKET)
                    continue;
                std::string host = tunnel.rule.targetHost;
                unsigned short port = tunnel.rule.targetPort;
                if (tunnel.rule.type == SshTunnelType::dynamic) {
                    SetSocksHandshakeTimeouts(local);
                    if (!SocksConnect(local, host, port)) {
                        SendSocksReply(local, 1);
                        CloseSocket(local);
                        continue;
                    }
                }
                SetTunnelSocketTimeouts(local);
                std::unique_ptr<ISshChannel> channel;
                channel = OpenDirectTcpip(session_, host, port);
                if (!channel) {
                    if (tunnel.rule.type == SshTunnelType::dynamic)
                        SendSocksReply(local, 5);
                    CloseSocket(local);
                    continue;
                }
                if (tunnel.rule.type == SshTunnelType::dynamic && !SendSocksReply(local, 0)) {
                    CloseSocket(local);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(tunnel.mutex);
                    tunnel.relaySockets.push_back(local);
                }
                tunnel.relayThreads.emplace_back(Relay, session_, local, std::move(channel), std::ref(tunnel.stop),
                                                 std::ref(tunnel.mutex), std::ref(tunnel.relaySockets));
            }
        });
    }
    tunnel.runtime.store(SshTunnelRuntimeState::running, std::memory_order_release);
    LogTunnelStarted(session_, tunnel.rule);
    return true;
}

bool SshTunnelManager::ReplaceRules(const std::vector<SshTunnelRule>& rules, std::string& error)
{
    std::lock_guard<std::mutex> managerLock(mutex_);
    std::vector<int> matches(rules.size(), -1);
    std::vector<bool> used(tunnels_.size(), false);
    const size_t common = (std::min)(tunnels_.size(), rules.size());
    for (size_t index = 0; index < common; ++index) {
        if (SameTunnelRule(tunnels_[index]->rule, rules[index])) {
            matches[index] = static_cast<int>(index);
            used[index] = true;
        }
    }
    for (size_t ruleIndex = 0; ruleIndex < rules.size(); ++ruleIndex) {
        if (matches[ruleIndex] >= 0)
            continue;
        for (size_t tunnelIndex = 0; tunnelIndex < tunnels_.size(); ++tunnelIndex) {
            if (!used[tunnelIndex] && SameTunnelRule(tunnels_[tunnelIndex]->rule, rules[ruleIndex])) {
                matches[ruleIndex] = static_cast<int>(tunnelIndex);
                used[tunnelIndex] = true;
                break;
            }
        }
    }
    for (size_t index = 0; index < tunnels_.size(); ++index) {
        if (used[index])
            continue;
        std::string ignored;
        StopTunnel(*tunnels_[index], ignored);
    }
    std::vector<std::unique_ptr<Tunnel>> updated;
    updated.reserve(rules.size());
    for (size_t index = 0; index < rules.size(); ++index) {
        if (matches[index] >= 0) {
            updated.push_back(std::move(tunnels_[static_cast<size_t>(matches[index])]));
            continue;
        }
        auto tunnel = std::make_unique<Tunnel>(rules[index]);
        if (tunnel->rule.startOnConnect && !StartTunnel(*tunnel, tunnel->error)) {
            if (error.empty())
                error = tunnel->error;
        }
        updated.push_back(std::move(tunnel));
    }
    tunnels_ = std::move(updated);
    return true;
}

void SshTunnelManager::StopAll() noexcept
{
    std::lock_guard<std::mutex> managerLock(mutex_);
    std::string ignored;
    for (const auto& tunnel : tunnels_)
        StopTunnel(*tunnel, ignored);
}

std::vector<SshTunnelStatus> SshTunnelManager::Status() const
{
    std::lock_guard<std::mutex> managerLock(mutex_);
    std::vector<SshTunnelStatus> result;
    result.reserve(tunnels_.size());
    for (const auto& tunnel : tunnels_) {
        result.push_back({ tunnel->rule.startOnConnect,
                           tunnel->runtime.load(std::memory_order_acquire),
                           tunnel->error });
    }
    return result;
}
