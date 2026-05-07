#include "global.h"
#include <windows.h>
#include <array>
#include <cerrno>
#include <memory>
#include <format>
#include <string>
#include "SftpClient.h"
#include "SftpInternal.h"
#include "SshBackendFactory.h"
#include "PluginEntryPoints.h"
#include "CoreUtils.h"
#include "res/resource.h"
#include "IUserFeedback.h"
#include "ITransportStream.h"
#include "SshSessionInit.h"

static LPVOID session_alloc(size_t count, LPVOID* /*abstract*/)
{
    return malloc(count);
}

static LPVOID session_realloc(LPVOID ptr, size_t count, LPVOID* /*abstract*/)
{
    return realloc(ptr, count);
}

static void session_free(LPVOID ptr, LPVOID* /*abstract*/)
{
    free(ptr);
}

// ---------------------------------------------------------------------------
// Custom SEND/RECV callbacks for transport-backed sessions (ProxyJump).
// These are installed before startup() when cs->transport_stream is set.
// The session abstract is the pConnectSettings pointer (set at createSession).
// ---------------------------------------------------------------------------
// libssh2's transport layer (transport.c:487-499) tests the recv/send return
// value with `nread == -EAGAIN` (POSIX errno), NOT `nread == -1 &&
// WSAGetLastError() == WSAEWOULDBLOCK`. Returning -1 here makes libssh2 treat
// would-block as a fatal socket error and abort KEX with
// LIBSSH2_ERROR_KEY_EXCHANGE_FAILURE. Return `-EAGAIN` (== -11) so libssh2
// recognises the would-block condition and propagates LIBSSH2_ERROR_EAGAIN up.
static ssize_t transport_send_cb(
    libssh2_socket_t /*sock*/,
    const void* buffer, size_t length,
    int /*flags*/,
    void** abstract)
{
    auto* cs = static_cast<tConnectSettings*>(*abstract);
    ssize_t rc = cs->transport_stream->write(buffer, length);
    SFTP_LOG("XPRT", "send len=%zu rc=%zd", length, rc);
    if (rc == ITRANSPORT_EAGAIN)
        return -EAGAIN;
    return rc;
}

static ssize_t transport_recv_cb(
    libssh2_socket_t /*sock*/,
    void* buffer, size_t length,
    int /*flags*/,
    void** abstract)
{
    auto* cs = static_cast<tConnectSettings*>(*abstract);
    ssize_t rc = cs->transport_stream->read(buffer, length);
    SFTP_LOG("XPRT", "recv len=%zu rc=%zd", length, rc);
    if (rc == ITRANSPORT_EAGAIN)
        return -EAGAIN;
    return rc;
}

int InitializeSshSession(
    pConnectSettings ConnectSettings,
    int& progress,
    int& loop,
    SYSTICKS& lasttime,
    std::unique_ptr<ISshBackend>& backend)
{
    std::array<char, 1024> buf{};
    progress = 30; // PROG_SESSION_INIT
    LoadStr(buf, IDS_INITSSH2);
    if (ProgressProc(PluginNumber, buf.data(), "-", progress))
        return -50;

    if (!backend)
        backend = CreateSshBackend();
    {
        auto sessionPtr = backend->createSession(session_alloc, session_free, session_realloc, ConnectSettings);
        if (!sessionPtr) {
            ShowErrorId(IDS_ERR_INIT_SSH2);
            if (LogProc) LogProc(PluginNumber, MSGTYPE_IMPORTANTERROR, "SFTP: SSH library session init failed");
            return -60;
        }
        ConnectSettings->session = std::move(sessionPtr);
    }

    ConnectSettings->session->setBlocking(0);

    // If we have a transport stream (ProxyJump), install custom SEND/RECV
    // callbacks so that libssh2 routes all I/O through the channel rather
    // than the underlying TCP socket directly.
    if (ConnectSettings->transport_stream) {
        ConnectSettings->session->callbackSet(
            LIBSSH2_CALLBACK_SEND, reinterpret_cast<void*>(transport_send_cb));
        ConnectSettings->session->callbackSet(
            LIBSSH2_CALLBACK_RECV, reinterpret_cast<void*>(transport_recv_cb));
        ShowStatus(("Transport: " + std::string(ConnectSettings->transport_stream->describe())).c_str());
    }

    loop = 30;
    LoadStr(buf, IDS_SET_COMPRESSION);
    LPCSTR ses_prefs = ConnectSettings->compressed ? "zlib,none" : "none";
    int err;
    while ((err = ConnectSettings->session->methodPref(LIBSSH2_METHOD_COMP_CS, ses_prefs)) == LIBSSH2_ERROR_EAGAIN) {
        if (ProgressLoop(buf.data(), progress, progress + 10, &loop, &lasttime))
            break;
        WaitForTransportReadable(ConnectSettings);
    }
    SftpLogLastError("libssh2_session_method_pref: ", err);

    while ((err = ConnectSettings->session->methodPref(LIBSSH2_METHOD_COMP_SC, ses_prefs)) == LIBSSH2_ERROR_EAGAIN) {
        if (ProgressLoop(buf.data(), progress, progress + 10, &loop, &lasttime))
            break;
        WaitForTransportReadable(ConnectSettings);
    }
    SftpLogLastError("libssh2_session_method_pref2: ", err);

    progress = 40; // PROG_SESSION_STARTUP
    LoadStr(buf, IDS_SESSION_STARTUP);
    SFTP_LOG("CONN", "Target session startup begin (transport=%s)",
             ConnectSettings->transport_stream
                 ? ConnectSettings->transport_stream->describe()
                 : "direct-socket");
    int auth;
    int eagainIters = 0;
    SYSTICKS startupT0 = get_sys_ticks();
    while ((auth = ConnectSettings->session->startup((int)ConnectSettings->sock)) == LIBSSH2_ERROR_EAGAIN) {
        ++eagainIters;
        if (ProgressLoop(buf.data(), progress, progress + 20, &loop, &lasttime))
            break;
        WaitForTransportReadable(ConnectSettings);
    }
    SFTP_LOG("CONN", "Target session startup done: rc=%d iters=%d elapsed=%ums",
             auth, eagainIters, (unsigned)get_ticks_between(startupT0));

    if (auth) {
        char* errmsg;
        int errmsg_len;
        ConnectSettings->session->lastError(&errmsg, &errmsg_len, false);
        SFTP_LOG("CONN", "Target session startup FAIL: errno=%d msg='%s'",
                 ConnectSettings->session->lastErrno(),
                 errmsg ? errmsg : "(no details)");
        ShowErrorId(IDS_ERR_SSH_SESSION, errmsg);
        if (LogProc) {
            const std::string msg = std::format("SFTP: SSH handshake failed: {}", errmsg ? errmsg : "(no details)");
            LogProc(PluginNumber, MSGTYPE_IMPORTANTERROR, msg.c_str());
        }
        return -70;
    }
    SftpLogLastError("libssh2_session_startup: ", ConnectSettings->session->lastErrno());
    return 0;
}

int VerifyServerFingerprint(pConnectSettings ConnectSettings)
{
    LPCSTR fingerprint = ConnectSettings->session->hostkeyHash(LIBSSH2_HOSTKEY_HASH_MD5);
    if (fingerprint == nullptr) {
        SftpLogLastError("Fingerprint error: ", ConnectSettings->session->lastErrno());
        return -90;
    }
    ShowStatusId(IDS_SERVER_FINGERPRINT, nullptr, true);
    std::string fingerprintHex;
    fingerprintHex.reserve(16 * 3);
    for (size_t i = 0; i < 16; i++) {
        if (i > 0) fingerprintHex += ' ';
        fingerprintHex += std::format("{:02X}", static_cast<unsigned char>(fingerprint[i]));
    }
    ShowStatus(fingerprintHex.c_str());

    if (ConnectSettings->savedfingerprint != fingerprintHex) {
        std::array<char, 4 * MAX_PATH> msg1{};
        std::array<char, MAX_PATH> msg2{};
        if (ConnectSettings->savedfingerprint.empty())
            LoadStr(msg1, IDS_CONNECTION_FIRSTTIME);
        else
            LoadStr(msg1, IDS_FINGERPRINT_CHANGED);
        LoadStr(msg2, IDS_FINGERPRINT);
        const std::string prompt = std::string(msg1.data()) + msg2.data() + fingerprintHex;

        bool verified = false;
        if (ConnectSettings->feedback) {
            verified = ConnectSettings->feedback->AskYesNo(prompt.c_str(), "SFTP Security Warning");
        } else {
            verified = false;
        }

        if (!verified)
            return -100;

        WritePrivateProfileString(ConnectSettings->DisplayName.c_str(), "fingerprint", fingerprintHex.c_str(), ConnectSettings->IniFileName.c_str());
        ConnectSettings->savedfingerprint = fingerprintHex;
    }
    return 0;
}
