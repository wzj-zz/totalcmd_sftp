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
#include "SshTunnelManager.h"

namespace {

constexpr int kTunnelBacklog = 16;
constexpr DWORD kTunnelPollMs = 100;
constexpr DWORD kRemoteForwardRetryMs = 3000;

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

void Relay(pConnectSettings session, SOCKET local, std::unique_ptr<ISshChannel> channel, std::atomic<bool>& stop)
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
    CloseSocket(local);
    // Channel destruction sends SSH close packets, so it must use the same
    // serialization as every other libssh2 call on this shared session.
    ScopedSshSessionUse use(session);
    channel.reset();
}

std::unique_ptr<ISshChannel> OpenDirectTcpip(pConnectSettings session, const std::string& host,
                                             unsigned short port)
{
    for (;;) {
        std::unique_ptr<ISshChannel> channel;
        int error = 0;
        {
            ScopedSshSessionUse use(session);
            channel = session->session->directTcpip(host.c_str(), port, "127.0.0.1", 0);
            error = channel ? 0 : session->session->lastErrno();
        }
        if (channel || error != LIBSSH2_ERROR_EAGAIN)
            return channel;
        if (!WaitForSshIo(session, kTunnelPollMs))
            return {};
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
    std::atomic<bool> stop{ false };
    std::atomic<bool> running{ false };
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
    bool allStarted = true;
    for (size_t index = 0; index < tunnels_.size(); ++index) {
        if (!tunnels_[index]->rule.startOnConnect)
            continue;
        std::string ruleError;
        if (!SetEnabled(index, true, ruleError)) {
            allStarted = false;
            if (error.empty())
                error = std::move(ruleError);
        }
    }
    return allStarted;
}

bool SshTunnelManager::SetEnabled(size_t index, bool enabled, std::string& error)
{
    if (!session_ || index >= tunnels_.size()) {
        error = "Tunnel rule is unavailable.";
        return false;
    }
    Tunnel& tunnel = *tunnels_[index];
    if (!enabled) {
        tunnel.stop.store(true, std::memory_order_release);
        CloseSocket(tunnel.localListener);
        if (tunnel.remoteListener) {
            ScopedSshSessionUse use(session_);
            while (tunnel.remoteListener->cancel() == LIBSSH2_ERROR_EAGAIN)
                WaitForSshIo(session_, kTunnelPollMs);
        }
        if (tunnel.listenerThread.joinable())
            tunnel.listenerThread.join();
        tunnel.remoteListener.reset();
        for (std::thread& worker : tunnel.relayThreads)
            if (worker.joinable()) worker.join();
        tunnel.relayThreads.clear();
        tunnel.running.store(false, std::memory_order_release);
        return true;
    }
    if (tunnel.running.load(std::memory_order_acquire))
        return true;
    tunnel.error.clear();
    tunnel.stop.store(false, std::memory_order_release);
    if (tunnel.rule.type == SshTunnelType::remote) {
        int boundPort = tunnel.rule.listenPort;
        tunnel.remoteListener = OpenForwardListener(session_, tunnel.rule, boundPort);
        if (!tunnel.remoteListener) {
            error = "SSH server refused remote tunnel " + FormatSshTunnelRule(tunnel.rule) +
                ". The remote listen port may still be releasing, already in use, or blocked by the server's remote forwarding policy.";
            tunnel.error = error;
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
                tunnel.relayThreads.emplace_back(Relay, session_, local, std::move(channel), std::ref(tunnel.stop));
            }
        });
    } else {
        tunnel.localListener = CreateListener(tunnel.rule.bindAddress, tunnel.rule.listenPort, error);
        if (tunnel.localListener == INVALID_SOCKET) {
            tunnel.error = error;
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
                    if (!SocksConnect(local, host, port)) {
                        SendSocksReply(local, 1);
                        CloseSocket(local);
                        continue;
                    }
                }
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
                tunnel.relayThreads.emplace_back(Relay, session_, local, std::move(channel), std::ref(tunnel.stop));
            }
        });
    }
    tunnel.running.store(true, std::memory_order_release);
    return true;
}

void SshTunnelManager::StopAll() noexcept
{
    std::string ignored;
    for (size_t index = 0; index < tunnels_.size(); ++index)
        SetEnabled(index, false, ignored);
}

std::vector<bool> SshTunnelManager::Running() const
{
    std::vector<bool> result;
    result.reserve(tunnels_.size());
    for (const auto& tunnel : tunnels_)
        result.push_back(tunnel->running.load(std::memory_order_acquire));
    return result;
}

std::vector<std::string> SshTunnelManager::Errors() const
{
    std::vector<std::string> result;
    result.reserve(tunnels_.size());
    for (const auto& tunnel : tunnels_)
        result.push_back(tunnel->error);
    return result;
}
