#pragma once

#include <memory>
#include <string>
#include <vector>
#include "SshTunnel.h"

struct tConnectSettings;

class SshTunnelManager {
public:
    explicit SshTunnelManager(tConnectSettings* session);
    ~SshTunnelManager();
    SshTunnelManager(const SshTunnelManager&) = delete;
    SshTunnelManager& operator=(const SshTunnelManager&) = delete;

    bool StartDefaults(std::string& error);
    bool SetEnabled(size_t index, bool enabled, std::string& error);
    void StopAll() noexcept;
    std::vector<bool> Running() const;
    std::vector<std::string> Errors() const;

private:
    struct Tunnel;
    tConnectSettings* session_;
    std::vector<std::unique_ptr<Tunnel>> tunnels_;
};
