#pragma once

#include <string_view>
#include "fsplugin.h"

void DispatchTunnelLog(int pluginNumber, int messageType, std::string_view message,
                       tLogProc ansiLog, tLogProcW wideLog) noexcept;
