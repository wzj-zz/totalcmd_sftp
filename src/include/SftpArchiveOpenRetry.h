#pragma once

#include <memory>
#include <utility>

#include "ISshBackend.h"

template <typename OpenFile, typename LastError, typename WaitForIo>
std::unique_ptr<ISftpHandle> OpenSftpArchiveFileWithRetry(const char* path, unsigned long flags, long mode,
                                                           OpenFile&& openFile, LastError&& lastError,
                                                           WaitForIo&& waitForIo)
{
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
        auto output = openFile(path, flags, mode);
        if (output)
            return output;
        const int error = lastError();
        if (error != LIBSSH2_ERROR_EAGAIN && error != 0)
            return {};
        if (!waitForIo())
            return {};
    }
    return {};
}
