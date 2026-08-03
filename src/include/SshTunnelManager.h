#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "SshTunnel.h"

struct tConnectSettings;

enum class SshTunnelRuntimeState : unsigned char {
    stopped,
    running,
    failed,
};

struct SshTunnelStatus {
    bool desired = false;
    SshTunnelRuntimeState runtime = SshTunnelRuntimeState::stopped;
    std::string error;
};

class SshTunnelManager {
public:
    explicit SshTunnelManager(tConnectSettings* session);
    ~SshTunnelManager();
    SshTunnelManager(const SshTunnelManager&) = delete;
    SshTunnelManager& operator=(const SshTunnelManager&) = delete;

    bool StartDefaults(std::string& error);
    bool SetEnabled(size_t index, bool enabled, std::string& error);
    bool ReplaceRules(const std::vector<SshTunnelRule>& rules, std::string& error);
    void StopAll() noexcept;
    std::vector<SshTunnelStatus> Status() const;

private:
    struct Tunnel;
    bool StartTunnel(Tunnel& tunnel, std::string& error);
    bool StopTunnel(Tunnel& tunnel, std::string& error) noexcept;
    tConnectSettings* session_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Tunnel>> tunnels_;
};
