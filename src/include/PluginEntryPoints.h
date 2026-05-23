#pragma once

#include "CoreUtils.h"
#include "fsplugin.h"
#include "SftpClient.h"

extern HINSTANCE hinst;
extern int PluginNumber;
extern int CryptoNumber;
extern DWORD mainthreadid;

extern tProgressProc  ProgressProc;
extern tProgressProcW ProgressProcW;
extern tLogProc       LogProc;
extern tLogProcW      LogProcW;
extern tRequestProc   RequestProc;
extern tRequestProcW  RequestProcW;
extern tCryptProc     CryptProc;

extern bool CryptCheckPass;

extern char pluginname[];

__forceinline
bool IsMainThread() noexcept
{
    return GetCurrentThreadId() == mainthreadid;
}

void LogMsg(LPCSTR fmt, ...) noexcept;
void ShowStatus(LPCSTR status) noexcept;
void ShowStatusW(LPCWSTR status) noexcept;
bool UpdatePercentBar(pConnectSettings ConnectSettings, int percent, LPCWSTR source = nullptr, LPCWSTR target = nullptr) noexcept;
void ApplyConfiguredUiLanguageForCurrentThread() noexcept;
LANGID GetConfiguredUiLanguageId() noexcept;

// Diagnostic prefix for status messages: when a guard is active in the
// current thread, ShowStatus* prepends "[<session>] " to the message so
// multi-session users can tell which connection is logging. Install at
// Fs*-entry points (or any function with pConnectSettings in scope) —
// propagates automatically to all nested calls on the same thread via
// thread-local storage. Restores previous context on scope exit (RAII),
// so nested guards for different sessions interleave correctly.
class SessionContextGuard {
public:
    explicit SessionContextGuard(const tConnectSettings* cs) noexcept;
    ~SessionContextGuard() noexcept;
    SessionContextGuard(const SessionContextGuard&) = delete;
    SessionContextGuard& operator=(const SessionContextGuard&) = delete;
private:
    const tConnectSettings* prev_;
};

// Current thread's active session for diagnostic prefixing, or nullptr.
const tConnectSettings* GetCurrentSession() noexcept;


