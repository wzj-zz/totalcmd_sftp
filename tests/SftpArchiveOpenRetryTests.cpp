#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "SftpArchiveOpenRetry.h"
#include "SshTunnel.h"
#include "TunnelLog.h"

namespace {

unsigned g_ansiLogCalls = 0;
unsigned g_wideLogCalls = 0;
int g_logPlugin = 0;
int g_logType = 0;
std::string g_ansiLogText;
std::wstring g_wideLogText;

void WINAPI CaptureAnsiLog(int plugin, int type, LPCSTR text)
{
    ++g_ansiLogCalls;
    g_logPlugin = plugin;
    g_logType = type;
    g_ansiLogText = text ? text : "";
}

void WINAPI CaptureWideLog(int plugin, int type, LPCWSTR text)
{
    ++g_wideLogCalls;
    g_logPlugin = plugin;
    g_logType = type;
    g_wideLogText = text ? text : L"";
}

void ResetLogCapture()
{
    g_ansiLogCalls = 0;
    g_wideLogCalls = 0;
    g_logPlugin = 0;
    g_logType = 0;
    g_ansiLogText.clear();
    g_wideLogText.clear();
}

class TestSftpHandle final : public ISftpHandle {
public:
    ssize_t read(char*, size_t) override { return 0; }
    ssize_t write(const char*, size_t) override { return 0; }
    int readdir(char*, size_t, char*, size_t, LIBSSH2_SFTP_ATTRIBUTES*) override { return 0; }
    int close() override { return 0; }
    void seek(size_t) override {}
    size_t tell() override { return 0; }
    int fstat(LIBSSH2_SFTP_ATTRIBUTES*, int) override { return 0; }
};

bool TestEagainRetriesTheSameStagingPath()
{
    constexpr char stagingPath[] = "/C:/archive/.sftp-archive-fixed.tmp";
    std::vector<std::string> openedPaths;
    unsigned waits = 0;
    auto output = OpenSftpArchiveFileWithRetry(
        stagingPath, LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, 0600,
        [&](const char* path, unsigned long, long) -> std::unique_ptr<ISftpHandle> {
            openedPaths.emplace_back(path);
            if (openedPaths.size() < 3)
                return {};
            return std::make_unique<TestSftpHandle>();
        },
        [] { return LIBSSH2_ERROR_EAGAIN; },
        [&] { return ++waits <= 2; });

    return output && waits == 2 && openedPaths.size() == 3 &&
        openedPaths[0] == stagingPath && openedPaths[1] == stagingPath && openedPaths[2] == stagingPath;
}

bool TestSshTunnelRules()
{
    struct Case {
        const char* text;
        SshTunnelType type;
        bool start;
        const char* bind;
        unsigned short listen;
        const char* target;
        unsigned short targetPort;
    };
    const Case cases[] = {
        { "+ -L 127.0.0.1:8080:app.internal:80", SshTunnelType::local, true, "127.0.0.1", 8080, "app.internal", 80 },
        { "- -R 0.0.0.0:2222:127.0.0.1:22", SshTunnelType::remote, false, "0.0.0.0", 2222, "127.0.0.1", 22 },
        { "+ -D [::1]:1080", SshTunnelType::dynamic, true, "::1", 1080, "", 0 },
        { "+ -L 8443:[2001:db8::10]:443", SshTunnelType::local, true, "127.0.0.1", 8443, "2001:db8::10", 443 },
    };
    std::vector<SshTunnelRule> rules;
    for (const Case& item : cases) {
        SshTunnelRule rule;
        std::string error;
        if (!ParseSshTunnelRule(item.text, rule, error) || rule.type != item.type ||
            rule.startOnConnect != item.start || rule.bindAddress != item.bind || rule.listenPort != item.listen ||
            rule.targetHost != item.target || rule.targetPort != item.targetPort)
            return false;
        rules.push_back(std::move(rule));
    }
    std::string error;
    SshTunnelRule invalid;
    const std::vector<SshTunnelRule> defaults = DefaultSshTunnelRules();
    return ValidateSshTunnelRules(rules, error) && defaults.size() == 3 &&
           FormatSshTunnelRule(defaults[0]) == "- -L 0.0.0.0:2260:127.0.0.1:2260" &&
           FormatSshTunnelRule(defaults[1]) == "- -R 0.0.0.0:1080:127.0.0.1:1080" &&
           FormatSshTunnelRule(defaults[2]) == "- -D 0.0.0.0:1081" &&
           !ParseSshTunnelRule("+ -D 0", invalid, error) &&
           !ParseSshTunnelRule("+ -L 8080:missing-port", invalid, error);
}

bool TestTunnelLogDispatch()
{
    ResetLogCapture();
    DispatchTunnelLog(7, MSGTYPE_DETAILS, "SFTP tunnel started", CaptureAnsiLog, CaptureWideLog);
    if (g_wideLogCalls != 1 || g_ansiLogCalls != 0 || g_logPlugin != 7 ||
        g_logType != MSGTYPE_DETAILS || g_wideLogText != L"SFTP tunnel started")
        return false;

    ResetLogCapture();
    DispatchTunnelLog(8, MSGTYPE_DETAILS, "SFTP tunnel stopped", CaptureAnsiLog, nullptr);
    if (g_ansiLogCalls != 1 || g_wideLogCalls != 0 || g_logPlugin != 8 ||
        g_logType != MSGTYPE_DETAILS || g_ansiLogText != "SFTP tunnel stopped")
        return false;

    ResetLogCapture();
    DispatchTunnelLog(9, MSGTYPE_IMPORTANTERROR, "SFTP tunnel startup failed", nullptr, CaptureWideLog);
    return g_wideLogCalls == 1 && g_ansiLogCalls == 0 && g_logPlugin == 9 &&
        g_logType == MSGTYPE_IMPORTANTERROR && g_wideLogText == L"SFTP tunnel startup failed";
}

} // namespace

int main()
{
    if (TestEagainRetriesTheSameStagingPath() && TestSshTunnelRules() && TestTunnelLogDispatch())
        return 0;
    std::fputs("SFTP archive retry, SSH tunnel rule, or tunnel log test failed.\n", stderr);
    return 1;
}
