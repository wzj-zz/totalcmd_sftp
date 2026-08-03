#include <windows.h>
#include <string>
#include "TunnelLog.h"

void DispatchTunnelLog(int pluginNumber, int messageType, std::string_view message,
                       tLogProc ansiLog, tLogProcW wideLog) noexcept
{
    if (wideLog) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()),
                                               nullptr, 0);
        if (length > 0) {
            std::wstring wide(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()),
                                wide.data(), length);
            wideLog(pluginNumber, messageType, wide.c_str());
            return;
        }
    }
    if (ansiLog) {
        const std::string text(message);
        ansiLog(pluginNumber, messageType, text.c_str());
    }
}
