#include "global.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>
#include "SshTunnel.h"

namespace {

std::string_view Trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

bool ParsePort(std::string_view value, unsigned short& port)
{
    unsigned number = 0;
    const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (status != std::errc{} || end != value.data() + value.size() || number == 0 || number > 65535)
        return false;
    port = static_cast<unsigned short>(number);
    return true;
}

bool SplitEndpoint(std::string_view text, std::vector<std::string_view>& parts)
{
    while (!text.empty()) {
        if (text.front() == '[') {
            const size_t close = text.find(']');
            if (close == std::string_view::npos)
                return false;
            parts.push_back(text.substr(1, close - 1));
            text.remove_prefix(close + 1);
            if (text.empty())
                return true;
            if (text.front() != ':')
                return false;
            text.remove_prefix(1);
            continue;
        }
        const size_t separator = text.find(':');
        parts.push_back(text.substr(0, separator));
        if (separator == std::string_view::npos)
            return true;
        text.remove_prefix(separator + 1);
    }
    return false;
}

std::string FormatHost(std::string_view host)
{
    return host.find(':') == std::string_view::npos ? std::string(host) : "[" + std::string(host) + "]";
}

}

bool ParseSshTunnelRule(const std::string& text, SshTunnelRule& rule, std::string& error)
{
    std::string_view value = Trim(text);
    if (value.size() < 4 || (value.front() != '+' && value.front() != '-') || value[1] != ' ' || value[2] != '-') {
        error = "Tunnel rules must start with '+ -L', '+ -R', or '+ -D'.";
        return false;
    }
    rule = {};
    rule.startOnConnect = value.front() == '+';
    const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(value[3])));
    if ((kind != 'L' && kind != 'R' && kind != 'D') || (value.size() > 4 && !std::isspace(static_cast<unsigned char>(value[4])))) {
        error = "Tunnel type must be -L, -R, or -D.";
        return false;
    }
    rule.type = kind == 'L' ? SshTunnelType::local : kind == 'R' ? SshTunnelType::remote : SshTunnelType::dynamic;
    value = Trim(value.substr(4));
    std::vector<std::string_view> parts;
    if (value.empty() || !SplitEndpoint(value, parts)) {
        error = "Invalid tunnel endpoint. Use [bind_address:]port[:host:port].";
        return false;
    }
    const size_t expected = rule.type == SshTunnelType::dynamic ? 1 : 3;
    if (parts.size() == expected + 1) {
        rule.bindAddress = std::string(parts.front());
        parts.erase(parts.begin());
    } else if (parts.size() == expected) {
        rule.bindAddress = rule.type == SshTunnelType::remote ? "127.0.0.1" : "127.0.0.1";
    } else {
        error = rule.type == SshTunnelType::dynamic
            ? "Dynamic tunnel must be -D [bind_address:]port."
            : "Local and remote tunnels must be -L/-R [bind_address:]port:host:port.";
        return false;
    }
    if (rule.bindAddress.empty() || !ParsePort(parts[0], rule.listenPort)) {
        error = "Tunnel listen address or port is invalid.";
        return false;
    }
    if (rule.type != SshTunnelType::dynamic) {
        if (parts[1].empty() || !ParsePort(parts[2], rule.targetPort)) {
            error = "Tunnel target host or port is invalid.";
            return false;
        }
        rule.targetHost = std::string(parts[1]);
    }
    return true;
}

std::string FormatSshTunnelRule(const SshTunnelRule& rule)
{
    const char kind = rule.type == SshTunnelType::local ? 'L' : rule.type == SshTunnelType::remote ? 'R' : 'D';
    std::string text = std::string(rule.startOnConnect ? "+ -" : "- -") + kind + " " +
        FormatHost(rule.bindAddress) + ":" + std::to_string(rule.listenPort);
    if (rule.type != SshTunnelType::dynamic)
        text += ":" + FormatHost(rule.targetHost) + ":" + std::to_string(rule.targetPort);
    return text;
}

bool ValidateSshTunnelRules(const std::vector<SshTunnelRule>& rules, std::string& error)
{
    for (size_t i = 0; i < rules.size(); ++i) {
        const SshTunnelRule& rule = rules[i];
        if (rule.listenPort == 0 || rule.bindAddress.empty() ||
            (rule.type != SshTunnelType::dynamic && (rule.targetHost.empty() || rule.targetPort == 0))) {
            error = "Tunnel rule " + std::to_string(i + 1) + " is incomplete.";
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            const SshTunnelRule& other = rules[j];
            if (rule.type != SshTunnelType::remote && other.type != SshTunnelType::remote &&
                _stricmp(rule.bindAddress.c_str(), other.bindAddress.c_str()) == 0 && rule.listenPort == other.listenPort) {
                error = "Tunnel rules " + std::to_string(j + 1) + " and " + std::to_string(i + 1) +
                    " use the same local listener.";
                return false;
            }
        }
    }
    return true;
}

std::vector<SshTunnelRule> DefaultSshTunnelRules()
{
    return {
        { SshTunnelType::local, false, "0.0.0.0", 2260, "127.0.0.1", 2260 },
        { SshTunnelType::remote, false, "0.0.0.0", 1080, "127.0.0.1", 1080 },
        { SshTunnelType::dynamic, false, "0.0.0.0", 1081, "", 0 },
    };
}
