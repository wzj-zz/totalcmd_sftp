#pragma once

#include <string>
#include <vector>

struct LocalDiffChanges {
    std::vector<std::wstring> deletions;
    std::vector<std::wstring> uploads;

    bool empty() const noexcept { return deletions.empty() && uploads.empty(); }
};

// Owns a temporary local mirror and its immutable baseline while TC compares
// the mirror with another local directory. It has no SFTP knowledge.
class LocalDiffSession {
public:
    bool Create(std::wstring& error);
    void Cleanup() noexcept;

    const std::wstring& leftMirror() const noexcept { return leftMirror_; }
    const std::wstring& rightMirror() const noexcept { return rightMirror_; }
    const std::wstring& root() const noexcept { return root_; }

    bool SnapshotLeft(std::wstring& error) const;
    bool SnapshotRight(std::wstring& error) const;
    bool CollectLeftChanges(LocalDiffChanges& changes, std::wstring& error) const;
    bool CollectRightChanges(LocalDiffChanges& changes, std::wstring& error) const;
    bool RunSynchronizeDirectories(const std::wstring& totalCommander, const std::wstring& left,
                                   const std::wstring& right, std::wstring& error) const;

private:
    bool Snapshot(const std::wstring& source, const std::wstring& baseline, std::wstring& error) const;
    bool CollectChanges(const std::wstring& baseline, const std::wstring& mirror,
                        LocalDiffChanges& changes, std::wstring& error) const;

    std::wstring root_;
    std::wstring leftMirror_;
    std::wstring rightMirror_;
    std::wstring leftBaseline_;
    std::wstring rightBaseline_;
};
