#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "SftpArchiveOpenRetry.h"

namespace {

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

} // namespace

int main()
{
    if (TestEagainRetriesTheSameStagingPath())
        return 0;
    std::fputs("EAGAIN retry changed the Windows archive staging path.\n", stderr);
    return 1;
}
