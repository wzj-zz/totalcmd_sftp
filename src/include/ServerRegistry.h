#pragma once

typedef LPVOID  SERVERID;
typedef LPVOID  SERVERHANDLE;

class ServerSessionLease {
public:
    ServerSessionLease() noexcept = default;
    ~ServerSessionLease();
    ServerSessionLease(const ServerSessionLease&) = delete;
    ServerSessionLease& operator=(const ServerSessionLease&) = delete;
    ServerSessionLease(ServerSessionLease&& other) noexcept;
    ServerSessionLease& operator=(ServerSessionLease&& other) noexcept;

    SERVERID get() const noexcept { return serverid_; }
    explicit operator bool() const noexcept { return serverid_ != nullptr; }

private:
    friend ServerSessionLease AcquireServerSessionLease(LPCSTR name) noexcept;
    friend ServerSessionLease AcquirePrimaryServerSessionLease(LPCSTR name) noexcept;
    explicit ServerSessionLease(LPVOID entry, SERVERID serverid) noexcept
        : entry_(entry), serverid_(serverid) {}
    void reset() noexcept;

    LPVOID entry_ = nullptr;
    SERVERID serverid_ = nullptr;
};

// Lifecycle
void InitMultiServer() noexcept;
void ShutdownMultiServer() noexcept;   // DeleteCriticalSection + FreeServerList
void FreeServerList() noexcept;

// Ini access — Unicode (W) variants accept a pre-computed wide path so that
// the ini file may live under a Unicode directory. The ANSI shims convert
// and delegate to the W variants; prefer the W forms in new code.
int  LoadServersFromIniW(LPCWSTR inifilename, LPCSTR quickconnectname) noexcept;
bool DeleteServerFromIniW(LPCSTR servername, LPCWSTR inifilename) noexcept;
int  CopyMoveServerInIniW(LPCSTR oldservername, LPCSTR newservername, bool Move, bool OverWrite, LPCWSTR inifilename) noexcept;

int  LoadServersFromIni(LPCSTR inifilename, LPCSTR quickconnectname) noexcept;
bool DeleteServerFromIni(LPCSTR servername, LPCSTR inifilename) noexcept;
int  CopyMoveServerInIni(LPCSTR oldservername, LPCSTR newservername, bool Move, bool OverWrite, LPCSTR inifilename) noexcept;

// Server id lookup / registration
SERVERID GetServerIdFromName(LPCSTR servername, DWORD threadid) noexcept;
// Finds an active connection by its display name regardless of the TC worker
// thread that owns it. Used when TC copies between two plugin panels.
SERVERID GetServerIdFromAnyThread(LPCSTR servername) noexcept;
// Keeps an active session alive until the returned lease is released. Use this
// for long-running cross-thread work instead of retaining a raw SERVERID.
ServerSessionLease AcquireServerSessionLease(LPCSTR servername) noexcept;
// Selects the primary Total Commander session, never a background transfer session.
ServerSessionLease AcquirePrimaryServerSessionLease(LPCSTR servername) noexcept;
bool SetServerIdForName(LPCSTR displayname, SERVERID newid) noexcept;

// Path helpers
void GetDisplayNameFromPath(LPCSTR Path, LPSTR DisplayName, size_t maxlen) noexcept;

SERVERHANDLE FindFirstServer(LPSTR displayname, size_t maxlen) noexcept;
SERVERHANDLE FindNextServer(SERVERHANDLE searchhandle, LPSTR displayname, size_t maxlen) noexcept;
void FindCloseServer(SERVERHANDLE searchhandle) noexcept;
