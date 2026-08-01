#pragma once

#include <string>
#include <vector>

enum class SshTunnelType : unsigned char {
    local,
    remote,
    dynamic,
};

struct SshTunnelRule {
    SshTunnelType type = SshTunnelType::local;
    bool startOnConnect = false;
    std::string bindAddress = "127.0.0.1";
    unsigned short listenPort = 0;
    std::string targetHost;
    unsigned short targetPort = 0;
};

// Parses a persisted rule in the form "+ -L [bind:]port:host:port".
bool ParseSshTunnelRule(const std::string& text, SshTunnelRule& rule, std::string& error);
std::string FormatSshTunnelRule(const SshTunnelRule& rule);
bool ValidateSshTunnelRules(const std::vector<SshTunnelRule>& rules, std::string& error);
std::vector<SshTunnelRule> DefaultSshTunnelRules();
