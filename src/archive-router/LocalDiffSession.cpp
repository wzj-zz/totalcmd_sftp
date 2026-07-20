#include "LocalDiffSession.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <string_view>
#include <vector>

namespace {

constexpr size_t kHashSize = 32;
constexpr ULONGLONG kStaleAge100Ns = 7ULL * 24 * 60 * 60 * 10'000'000;

struct LocalEntry {
    bool directory = false;
    uintmax_t size = 0;
    std::array<BYTE, kHashSize> hash{};
};

using LocalTree = std::map<std::wstring, LocalEntry, std::less<>>;

std::wstring JoinPath(const std::wstring& base, const std::wstring& name)
{
    return (std::filesystem::path(base) / name).wstring();
}

bool IsReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool HashFile(const std::filesystem::path& path, std::array<BYTE, kHashSize>& hash, std::wstring& error)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE handle = nullptr;
    DWORD objectLength = 0;
    DWORD bytes = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &bytes, 0) < 0) {
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"Could not initialize the SHA-256 provider.";
        return false;
    }
    std::vector<BYTE> object(objectLength);
    if (BCryptCreateHash(algorithm, &handle, object.data(), objectLength, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"Could not initialize a SHA-256 hash.";
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        BCryptDestroyHash(handle);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"Could not read " + path.wstring() + L".";
        return false;
    }
    std::array<BYTE, 256 * 1024> buffer{};
    DWORD count = 0;
    bool ok = true;
    for (;;) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            ok = false;
            break;
        }
        if (count == 0)
            break;
        if (BCryptHashData(handle, buffer.data(), count, 0) < 0) {
            ok = false;
            break;
        }
    }
    CloseHandle(file);
    if (ok)
        ok = BCryptFinishHash(handle, hash.data(), static_cast<ULONG>(hash.size()), 0) >= 0;
    BCryptDestroyHash(handle);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok)
        error = L"Could not calculate SHA-256 for " + path.wstring() + L".";
    return ok;
}

bool BuildTree(const std::wstring& root, LocalTree& tree, std::wstring& error)
{
    std::error_code fsError;
    for (std::filesystem::recursive_directory_iterator it(root, fsError), end; !fsError && it != end; it.increment(fsError)) {
        const std::filesystem::directory_entry& entry = *it;
        if (IsReparsePoint(entry.path())) {
            error = L"Local diff mirrors cannot contain links or reparse points: " + entry.path().wstring();
            return false;
        }
        const auto relative = std::filesystem::relative(entry.path(), root, fsError).generic_wstring();
        if (fsError || relative.empty() || relative.starts_with(L"..")) {
            error = L"Could not enumerate the local diff mirror.";
            return false;
        }
        LocalEntry item;
        item.directory = entry.is_directory(fsError);
        if (fsError) {
            error = L"Could not inspect " + entry.path().wstring() + L".";
            return false;
        }
        if (!item.directory) {
            if (!entry.is_regular_file(fsError)) {
                error = L"Local diff mirrors support only regular files and directories.";
                return false;
            }
            item.size = entry.file_size(fsError);
            if (fsError || !HashFile(entry.path(), item.hash, error))
                return false;
        }
        tree.emplace(relative, std::move(item));
    }
    if (fsError) {
        error = L"Could not enumerate the local diff mirror.";
        return false;
    }
    return true;
}

bool CopyTree(const std::wstring& source, const std::wstring& destination, std::wstring& error)
{
    std::error_code fsError;
    std::filesystem::remove_all(destination, fsError);
    fsError.clear();
    std::filesystem::create_directories(destination, fsError);
    if (fsError) {
        error = L"Could not create the local diff baseline.";
        return false;
    }
    for (std::filesystem::recursive_directory_iterator it(source, fsError), end; !fsError && it != end; it.increment(fsError)) {
        const auto relative = std::filesystem::relative(it->path(), source, fsError);
        if (fsError || relative.empty() || relative.native().starts_with(L"..") || IsReparsePoint(it->path())) {
            error = L"The local diff mirror contains an unsupported path.";
            return false;
        }
        const auto output = std::filesystem::path(destination) / relative;
        if (it->is_directory(fsError))
            std::filesystem::create_directories(output, fsError);
        else if (it->is_regular_file(fsError)) {
            std::filesystem::create_directories(output.parent_path(), fsError);
            if (!fsError)
                std::filesystem::copy_file(it->path(), output, std::filesystem::copy_options::overwrite_existing, fsError);
        } else {
            error = L"The local diff mirror contains an unsupported item.";
            return false;
        }
        if (fsError) {
            error = L"Could not save the local diff baseline.";
            return false;
        }
    }
    if (fsError) {
        error = L"Could not save the local diff baseline.";
        return false;
    }
    return true;
}

void RemoveStaleSessions(const std::wstring& base) noexcept
{
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER nowValue{ now.dwLowDateTime, now.dwHighDateTime };
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(base, error)) {
        if (error || !entry.is_directory(error))
            continue;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &data))
            continue;
        ULARGE_INTEGER age{ data.ftLastWriteTime.dwLowDateTime, data.ftLastWriteTime.dwHighDateTime };
        if (nowValue.QuadPart > age.QuadPart && nowValue.QuadPart - age.QuadPart > kStaleAge100Ns) {
            std::error_code removeError;
            std::filesystem::remove_all(entry.path(), removeError);
        }
    }
}

void ReduceDeletionPaths(std::vector<std::wstring>& paths)
{
    std::sort(paths.begin(), paths.end(), [](const std::wstring& left, const std::wstring& right) {
        if (left.size() != right.size())
            return left.size() < right.size();
        return left < right;
    });
    std::vector<std::wstring> reduced;
    for (const std::wstring& path : paths) {
        const bool child = std::any_of(reduced.begin(), reduced.end(), [&](const std::wstring& parent) {
            return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == L'/';
        });
        if (!child)
            reduced.push_back(path);
    }
    paths = std::move(reduced);
}

struct SyncWindowSearch {
    const std::vector<HWND>* existing = nullptr;
    HWND found = nullptr;
};

BOOL CALLBACK FindNewSyncWindow(HWND window, LPARAM parameter)
{
    auto& search = *reinterpret_cast<SyncWindowSearch*>(parameter);
    if (!IsWindowVisible(window))
        return TRUE;
    std::array<wchar_t, 32> className{};
    if (GetClassNameW(window, className.data(), static_cast<int>(className.size())) <= 0 ||
        wcscmp(className.data(), L"TCmpForm") != 0)
        return TRUE;
    if (std::find(search.existing->begin(), search.existing->end(), window) == search.existing->end()) {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

std::vector<HWND> GetSynchronizeDirectoriesWindows()
{
    std::vector<HWND> windows;
    EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        if (!IsWindowVisible(window))
            return TRUE;
        std::array<wchar_t, 32> className{};
        if (GetClassNameW(window, className.data(), static_cast<int>(className.size())) > 0 &&
            wcscmp(className.data(), L"TCmpForm") == 0)
            reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

HWND WaitForNewSynchronizeDirectoriesWindow(const std::vector<HWND>& existing)
{
    SyncWindowSearch search{ &existing };
    const ULONGLONG deadline = GetTickCount64() + 15'000;
    do {
        search.found = nullptr;
        EnumWindows(FindNewSyncWindow, reinterpret_cast<LPARAM>(&search));
        if (search.found)
            return search.found;
        Sleep(50);
    } while (GetTickCount64() < deadline);
    return nullptr;
}

} // namespace

bool LocalDiffSession::Create(std::wstring& error)
{
    std::array<wchar_t, MAX_PATH> temp{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (length == 0 || length >= temp.size()) {
        error = L"Could not determine the Windows temporary directory.";
        return false;
    }
    const std::wstring base = JoinPath(temp.data(), L"SftpLocalDiff");
    std::error_code fsError;
    std::filesystem::create_directories(base, fsError);
    if (fsError) {
        error = L"Could not create the SFTP local diff temporary directory.";
        return false;
    }
    RemoveStaleSessions(base);
    root_ = JoinPath(base, std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    leftMirror_ = JoinPath(root_, L"left");
    rightMirror_ = JoinPath(root_, L"right");
    leftBaseline_ = JoinPath(root_, L"baseline-left");
    rightBaseline_ = JoinPath(root_, L"baseline-right");
    std::filesystem::create_directories(leftMirror_, fsError);
    std::filesystem::create_directories(rightMirror_, fsError);
    if (fsError) {
        Cleanup();
        error = L"Could not create the SFTP local diff session.";
        return false;
    }
    return true;
}

void LocalDiffSession::Cleanup() noexcept
{
    if (root_.empty())
        return;
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    root_.clear();
}

bool LocalDiffSession::Snapshot(const std::wstring& source, const std::wstring& baseline, std::wstring& error) const
{
    return CopyTree(source, baseline, error);
}

bool LocalDiffSession::SnapshotLeft(std::wstring& error) const
{
    return Snapshot(leftMirror_, leftBaseline_, error);
}

bool LocalDiffSession::SnapshotRight(std::wstring& error) const
{
    return Snapshot(rightMirror_, rightBaseline_, error);
}

bool LocalDiffSession::CollectChanges(const std::wstring& baseline, const std::wstring& mirror,
                                       LocalDiffChanges& changes, std::wstring& error) const
{
    LocalTree before;
    LocalTree after;
    if (!BuildTree(baseline, before, error) || !BuildTree(mirror, after, error))
        return false;
    changes = {};
    for (const auto& [path, item] : before) {
        const auto found = after.find(path);
        if (found == after.end() || found->second.directory != item.directory)
            changes.deletions.push_back(path);
    }
    for (const auto& [path, item] : after) {
        const auto found = before.find(path);
        if (found == before.end() || found->second.directory != item.directory ||
            (!item.directory && (found->second.size != item.size || found->second.hash != item.hash))) {
            changes.uploads.push_back(path);
        }
    }
    ReduceDeletionPaths(changes.deletions);
    return true;
}

bool LocalDiffSession::CollectLeftChanges(LocalDiffChanges& changes, std::wstring& error) const
{
    return CollectChanges(leftBaseline_, leftMirror_, changes, error);
}

bool LocalDiffSession::CollectRightChanges(LocalDiffChanges& changes, std::wstring& error) const
{
    return CollectChanges(rightBaseline_, rightMirror_, changes, error);
}

bool LocalDiffSession::RunSynchronizeDirectories(const std::wstring& totalCommander, const std::wstring& left,
                                                   const std::wstring& right, std::wstring& error) const
{
    std::wstring command = L"\"" + totalCommander + L"\" /S=S:= \"" + left + L"\" \"" + right + L"\"";
    const std::vector<HWND> existingWindows = GetSynchronizeDirectoriesWindows();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(totalCommander.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process)) {
        error = L"Could not open Total Commander's Synchronize Directories window.";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    HWND dialog = WaitForNewSynchronizeDirectoriesWindow(existingWindows);
    if (!dialog) {
        error = L"Total Commander's Synchronize Directories window did not open.";
        return false;
    }
    while (IsWindow(dialog))
        Sleep(50);
    return true;
}
