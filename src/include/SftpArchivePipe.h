#pragma once

#include <windows.h>
#include <vector>
#include "SftpClient.h"

// Local IPC endpoint used by SftpArchiveRouter.exe. The router owns local
// tar.exe; this service owns the already-connected SSH session.
void StartSftpArchivePipeService() noexcept;
void StopSftpArchivePipeService() noexcept;

// Alt+F11 prewarms this cache with one remote find command. WFX directory
// enumeration uses it without changing Total Commander's sync workflow.
bool TryGetSftpManifestDirectoryListing(pConnectSettings cs, LPCWSTR remoteDir,
                                        std::vector<WIN32_FIND_DATAW>& entries) noexcept;
void InvalidateSftpManifestCache(pConnectSettings cs) noexcept;
