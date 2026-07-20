#include <windows.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "LocalDiffSession.h"

namespace {

constexpr DWORD kArchiveMagic = 0x41504653; // "SFPA"
constexpr DWORD kArchivePut = 1;
constexpr DWORD kArchiveGet = 2;
constexpr DWORD kArchivePack = 3;
constexpr DWORD kArchivePackToRemote = 4;
constexpr DWORD kArchiveExtract = 5;
constexpr DWORD kArchivePackExtractRemote = 6;
constexpr DWORD kArchiveIsDirectory = 7;
constexpr DWORD kArchiveDeleteRemote = 8;
constexpr DWORD kArchivePrewarmManifest = 9;
constexpr DWORD kArchiveLogStatus = 10;
constexpr DWORD kArchiveShowError = 11;
constexpr DWORD kChunkSize = 64 * 1024;
constexpr int kOperationCanceled = 3;

struct ArchiveRequest {
    DWORD magic;
    DWORD operation;
    DWORD sourcePathBytes;
    DWORD targetPathBytes;
    DWORD itemBytes;
};

std::string ToUtf8(std::wstring_view text);
HANDLE OpenArchivePipe();
bool StartArchiveRequest(HANDLE pipe, DWORD operation, std::wstring_view sourcePath,
                         std::wstring_view targetPath, std::string_view items,
                         std::string& error);

HWND FindTotalCommanderWindow()
{
    HWND tcWindow = FindWindowW(L"TTOTAL_CMD64", nullptr);
    return tcWindow ? tcWindow : FindWindowW(L"TTOTAL_CMD", nullptr);
}

int ShowRouterMessage(std::wstring_view message, std::wstring_view title, UINT flags)
{
    HWND owner = FindTotalCommanderWindow();
    return MessageBoxW(owner, std::wstring(message).c_str(), std::wstring(title).c_str(),
                       flags | MB_SETFOREGROUND);
}

bool ShowErrorInTotalCommander(std::wstring_view message)
{
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE)
        return false;
    std::string error;
    const bool shown = StartArchiveRequest(pipe, kArchiveShowError, L"", L"", ToUtf8(message), error);
    CloseHandle(pipe);
    return shown;
}

void ShowError(std::wstring_view message)
{
    if (!ShowErrorInTotalCommander(message))
        ShowRouterMessage(message, L"SFTP Archive Router", MB_OK | MB_ICONERROR);
}

void ShowInfo(std::wstring_view message)
{
    ShowRouterMessage(message, L"SFTP Archive Router", MB_OK | MB_ICONINFORMATION);
}

constexpr int kTargetPromptEdit = 1001;
constexpr int kTargetPromptHint = 1002;

struct TargetPromptState {
    const wchar_t* label;
    std::wstring* value;
};

INT_PTR CALLBACK TargetPromptProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INITDIALOG) {
        auto* state = reinterpret_cast<TargetPromptState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        SetDlgItemTextW(dialog, 1000, state->label);
        SetDlgItemTextW(dialog, kTargetPromptHint, L"Press Enter to use this value, or edit it before continuing.");
        SetDlgItemTextW(dialog, kTargetPromptEdit, state->value->c_str());
        SendDlgItemMessageW(dialog, kTargetPromptEdit, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, kTargetPromptEdit));
        return FALSE;
    }
    if (message == WM_CTLCOLORDLG) {
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
    }
    if (message == WM_CTLCOLORSTATIC) {
        const HDC dc = reinterpret_cast<HDC>(wParam);
        if (GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == kTargetPromptHint) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        }
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == IDOK) {
            auto* state = reinterpret_cast<TargetPromptState*>(GetWindowLongPtrW(dialog, DWLP_USER));
            const int length = GetWindowTextLengthW(GetDlgItem(dialog, kTargetPromptEdit));
            state->value->resize(static_cast<size_t>(length) + 1);
            GetDlgItemTextW(dialog, kTargetPromptEdit, state->value->data(), length + 1);
            state->value->resize(static_cast<size_t>(length));
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}

void AlignDialogTemplate(BYTE*& cursor)
{
    cursor = reinterpret_cast<BYTE*>((reinterpret_cast<uintptr_t>(cursor) + 3) & ~static_cast<uintptr_t>(3));
}

void AppendDialogWord(BYTE*& cursor, WORD value)
{
    *reinterpret_cast<WORD*>(cursor) = value;
    cursor += sizeof(value);
}

void AppendDialogText(BYTE*& cursor, const wchar_t* text)
{
    do {
        AppendDialogWord(cursor, static_cast<WORD>(*text));
    } while (*text++);
}

void AppendDialogItem(BYTE*& cursor, short x, short y, short width, short height, WORD id,
                      DWORD style, WORD controlClass, const wchar_t* text)
{
    AlignDialogTemplate(cursor);
    auto* item = reinterpret_cast<DLGITEMTEMPLATE*>(cursor);
    item->style = style;
    item->dwExtendedStyle = 0;
    item->x = x;
    item->y = y;
    item->cx = width;
    item->cy = height;
    item->id = id;
    cursor += sizeof(*item);
    AppendDialogWord(cursor, 0xFFFF);
    AppendDialogWord(cursor, controlClass);
    AppendDialogText(cursor, text);
    AppendDialogWord(cursor, 0);
}

bool PromptForTarget(const wchar_t* label, std::wstring& target)
{
    alignas(DWORD) std::array<BYTE, 1024> templateBytes{};
    BYTE* cursor = templateBytes.data();
    auto* header = reinterpret_cast<DLGTEMPLATE*>(cursor);
    header->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SHELLFONT;
    header->dwExtendedStyle = 0;
    header->cdit = 4;
    header->x = 10;
    header->y = 10;
    header->cx = 310;
    header->cy = 86;
    cursor += sizeof(*header);
    AppendDialogWord(cursor, 0);
    AppendDialogWord(cursor, 0);
    AppendDialogText(cursor, L"SFTP Archive Router");
    AppendDialogWord(cursor, 9);
    AppendDialogText(cursor, L"Segoe UI");
    AppendDialogItem(cursor, 10, 9, 288, 10, 1000, WS_CHILD | WS_VISIBLE, 0x0082, L"");
    AppendDialogItem(cursor, 10, 21, 288, 9, kTargetPromptHint, WS_CHILD | WS_VISIBLE, 0x0082, L"");
    AppendDialogItem(cursor, 10, 36, 288, 15, kTargetPromptEdit,
                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0x0081, L"");
    AppendDialogItem(cursor, 146, 61, 72, 16, IDOK,
                     WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0x0080, L"OK");
    AppendDialogItem(cursor, 226, 61, 72, 16, IDCANCEL,
                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0x0080, L"Cancel");

    TargetPromptState state{ label, &target };
    HWND owner = FindTotalCommanderWindow();
    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), header, owner,
                                                    TargetPromptProc, reinterpret_cast<LPARAM>(&state));
    if (result == -1) {
        ShowError(L"Could not show the target name dialog.");
        return false;
    }
    return result == IDOK && !target.empty();
}

std::wstring TargetForPrompt(std::wstring target)
{
    if (target.size() >= 2 && (target.ends_with(L"\\.") || target.ends_with(L"/.")))
        target.pop_back();
    return target;
}

std::wstring NormalizeLocalDirectory(std::wstring path)
{
    std::array<wchar_t, 32768> fullPath{};
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(fullPath.size()), fullPath.data(), nullptr);
    if (length == 0 || length >= fullPath.size())
        return path;
    return std::wstring(fullPath.data(), length);
}

bool EnsureWritableLocalDirectory(const std::wstring& directory, std::wstring& error)
{
    const std::wstring normalized = NormalizeLocalDirectory(directory);
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        error = L"The local target directory does not exist: " + normalized;
        return false;
    }
    std::array<wchar_t, MAX_PATH> probe{};
    if (!GetTempFileNameW(normalized.c_str(), L"SFR", 0, probe.data())) {
        error = L"The local target directory is not writable: " + normalized +
                L" (Windows error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    DeleteFileW(probe.data());
    return true;
}

bool EnsureWritableLocalFileTarget(const std::wstring& filePath, std::wstring& error)
{
    const std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
    if (parent.empty()) {
        error = L"The local archive target has no parent directory: " + filePath;
        return false;
    }
    return EnsureWritableLocalDirectory(parent.wstring(), error);
}

std::wstring QuoteWindowsArgument(std::wstring_view argument)
{
    std::wstring quoted(L"\"");
    size_t backslashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    // Double all terminal backslashes so they cannot escape the closing quote.
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

bool IsUncPath(std::wstring_view path)
{
    return path.size() >= 2 && ((path[0] == L'\\' && path[1] == L'\\') ||
                                (path[0] == L'/' && path[1] == L'/'));
}

struct LocalTarInvocation {
    std::wstring executable;
    std::wstring command;
};

LocalTarInvocation CreateLocalTarInvocation(const std::wstring& tarPath, const std::wstring& directory,
                                             std::wstring_view beforeDirectory, std::wstring_view afterDirectory)
{
    if (!IsUncPath(directory)) {
        return { tarPath, QuoteWindowsArgument(tarPath) + std::wstring(beforeDirectory) +
                 QuoteWindowsArgument(directory) + std::wstring(afterDirectory) };
    }

    std::array<wchar_t, MAX_PATH> systemDirectory{};
    const UINT length = GetSystemDirectoryW(systemDirectory.data(), static_cast<UINT>(systemDirectory.size()));
    if (length == 0 || length >= systemDirectory.size())
        return {};
    const std::wstring commandProcessor = std::wstring(systemDirectory.data(), length) + L"\\cmd.exe";
    // pushd gives every UNC provider a process-local drive mapping, including
    // WSL and SMB shares. The mapping disappears when cmd.exe exits.
    const std::wstring payload = L"pushd " + QuoteWindowsArgument(directory) + L" && " +
        QuoteWindowsArgument(tarPath) + std::wstring(beforeDirectory) + L"." +
        std::wstring(afterDirectory) + L" && popd";
    // cmd.exe needs its own outer command quotes. Do not apply CreateProcess
    // backslash escaping to payload because cmd.exe does not interpret \".
    return { commandProcessor, QuoteWindowsArgument(commandProcessor) + L" /d /v:off /s /c \"" + payload + L"\"" };
}

std::wstring GetExecutableDirectory();

std::wstring GetTotalCommanderExecutable()
{
    HWND tcWindow = FindTotalCommanderWindow();
    DWORD processId = 0;
    if (tcWindow)
        GetWindowThreadProcessId(tcWindow, &processId);
    HANDLE process = processId ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId) : nullptr;
    if (process) {
        std::array<wchar_t, 32768> path{};
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &length)) {
            CloseHandle(process);
            return std::wstring(path.data(), length);
        }
        CloseHandle(process);
    }
    const std::filesystem::path pluginDirectory(GetExecutableDirectory());
    const std::filesystem::path totalCommander = pluginDirectory.parent_path().parent_path().parent_path() / L"TOTALCMD64.EXE";
    return GetFileAttributesW(totalCommander.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring{} : totalCommander.wstring();
}

bool IsRightPanelActive()
{
    HWND tcWindow = FindTotalCommanderWindow();
    DWORD_PTR activePanel = 0;
    if (!tcWindow || !SendMessageTimeoutW(tcWindow, WM_USER + 50, 1000, 0,
                                          SMTO_ABORTIFHUNG, 2000, &activePanel))
        return false;
    return activePanel == 2;
}

bool IsSftpVirtualPath(std::wstring_view path)
{
    // TC's %P/%T expansion supplies this WFX as \\SFTP\... or \\\SFTP\...
    // depending on the operation. Only the first non-slash segment is tested.
    const size_t firstSegment = path.find_first_not_of(L"\\/");
    if (firstSegment == std::wstring_view::npos)
        return false;
    const size_t separator = path.find_first_of(L"\\/", firstSegment);
    const std::wstring_view root = separator == std::wstring_view::npos
        ? path.substr(firstSegment)
        : path.substr(firstSegment, separator - firstSegment);
    return _wcsicmp(std::wstring(root).c_str(), L"SFTP") == 0;
}

std::wstring DescribeOperationPath(std::wstring_view path)
{
    if (!IsSftpVirtualPath(path))
        return path.empty() ? std::wstring{} : L"local";
    const size_t root = path.find_first_not_of(L"\\/");
    const size_t rootEnd = path.find_first_of(L"\\/", root);
    const size_t session = rootEnd == std::wstring_view::npos ? std::wstring_view::npos
        : path.find_first_not_of(L"\\/", rootEnd);
    if (session == std::wstring_view::npos)
        return L"SFTP";
    const size_t sessionEnd = path.find_first_of(L"\\/", session);
    return std::wstring(path.substr(session, sessionEnd == std::wstring_view::npos ? std::wstring_view::npos : sessionEnd - session));
}

bool RunNativeTcCommand(const wchar_t* command)
{
    HWND tcWindow = FindTotalCommanderWindow();
    if (!tcWindow) {
        ShowError(L"Could not find the Total Commander main window to run the native archive command.");
        return false;
    }

    const int length = WideCharToMultiByte(CP_ACP, 0, command, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        ShowError(L"Could not encode the Total Commander archive command.");
        return false;
    }
    std::vector<char> commandA(static_cast<size_t>(length));
    WideCharToMultiByte(CP_ACP, 0, command, -1, commandA.data(), length, nullptr, nullptr);

    COPYDATASTRUCT data{};
    data.dwData = 0x4D45; // TC's documented WM_COPYDATA command marker.
    data.cbData = static_cast<DWORD>(commandA.size());
    data.lpData = commandA.data();
    // TC returns 0 for this message even after it has queued the command.
    // Delivery is synchronous, but command execution itself is deferred.
    SendMessageW(tcWindow, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
    return true;
}

bool ReadExact(HANDLE handle, void* data, DWORD length)
{
    auto* out = static_cast<BYTE*>(data);
    while (length > 0) {
        DWORD got = 0;
        if (!ReadFile(handle, out, length, &got, nullptr) || got == 0)
            return false;
        out += got;
        length -= got;
    }
    return true;
}

bool WriteExact(HANDLE handle, const void* data, DWORD length)
{
    const auto* input = static_cast<const BYTE*>(data);
    while (length > 0) {
        DWORD written = 0;
        if (!WriteFile(handle, input, length, &written, nullptr) || written == 0)
            return false;
        input += written;
        length -= written;
    }
    return true;
}

bool ReadResult(HANDLE pipe, std::string& error)
{
    DWORD code = 1;
    DWORD textBytes = 0;
    if (!ReadExact(pipe, &code, sizeof(code)) || !ReadExact(pipe, &textBytes, sizeof(textBytes)) || textBytes > 4096) {
        error = "The SFTP archive service did not return a valid result.";
        return false;
    }
    error.assign(textBytes, '\0');
    if (textBytes && !ReadExact(pipe, error.data(), textBytes)) {
        error = "The SFTP archive service disconnected.";
        return false;
    }
    return code == 0;
}

std::string ToUtf8(std::wstring_view text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
        return L"Archive operation failed.";
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

bool ReadTextLines(const std::wstring& path, std::vector<std::wstring>& lines)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    CloseHandle(file);
    if (!ok)
        return false;
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        if ((bytes.size() - 2) % sizeof(wchar_t) != 0)
            return false;
        text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / sizeof(wchar_t));
    } else {
        const UINT codepage = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
                              static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF
            ? CP_UTF8 : CP_ACP;
        const size_t offset = codepage == CP_UTF8 ? 3 : 0;
        const int length = MultiByteToWideChar(codepage, 0, bytes.data() + offset,
                                               static_cast<int>(bytes.size() - offset), nullptr, 0);
        if (length <= 0)
            return false;
        text.resize(static_cast<size_t>(length));
        MultiByteToWideChar(codepage, 0, bytes.data() + offset, static_cast<int>(bytes.size() - offset), text.data(), length);
    }
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find_first_of(L"\r\n", start);
        if (end > start)
            lines.emplace_back(text.substr(start, end - start));
        if (end == std::wstring::npos)
            break;
        start = end + 1;
        if (start < text.size() && text[end] == L'\r' && text[start] == L'\n')
            ++start;
    }
    return !lines.empty();
}

bool ReadRelativeTarNames(const std::wstring& sourcePath, const std::wstring& selectedList, std::vector<std::wstring>& relativeNames)
{
    std::vector<std::wstring> selected;
    if (!ReadTextLines(selectedList, selected))
        return false;
    relativeNames.reserve(selected.size());
    if (!IsSftpVirtualPath(sourcePath)) {
        std::filesystem::path root(sourcePath);
        root = root.lexically_normal();
        for (const std::wstring& item : selected) {
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::relative(std::filesystem::path(item), root, error);
            if (error || relative.empty() || relative.native().starts_with(L".."))
                return false;
            const std::wstring name = relative.generic_wstring();
            if (name.empty() || name.find_first_of(L"\r\n\"") != std::wstring::npos)
                return false;
            relativeNames.push_back(name);
        }
        return true;
    }

    std::wstring root(sourcePath);
    std::replace(root.begin(), root.end(), L'/', L'\\');
    if (root.size() >= 2 && root.ends_with(L"\\."))
        root.pop_back();
    while (root.size() > 1 && root.back() == L'\\')
        root.pop_back();
    for (const std::wstring& item : selected) {
        std::wstring normalized(item);
        std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        std::wstring name;
        if (normalized.size() > root.size() && _wcsnicmp(normalized.c_str(), root.c_str(), root.size()) == 0 &&
            normalized[root.size()] == L'\\') {
            name = normalized.substr(root.size() + 1);
        } else if (normalized.find_first_of(L"\\") == std::wstring::npos) {
            name = normalized;
        } else {
            return false;
        }
        if (name.empty() || name.starts_with(L"..") || name.find_first_of(L"\r\n\"") != std::wstring::npos)
            return false;
        std::replace(name.begin(), name.end(), L'\\', L'/');
        relativeNames.push_back(std::move(name));
    }
    return true;
}

std::string JoinArchiveItems(const std::vector<std::wstring>& names)
{
    std::string items;
    for (const std::wstring& name : names)
        items += ToUtf8(name) + "\n";
    return items;
}

std::wstring ArchiveName(const std::vector<std::wstring>& names)
{
    if (names.size() != 1)
        return L"@sftp.tar";
    std::wstring name = names.front();
    while (!name.empty() && (name.back() == L'/' || name.back() == L'\\'))
        name.pop_back();
    const std::wstring base = std::filesystem::path(name).filename().wstring();
    return (base.empty() ? L"@sftp" : base) + L".tar";
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& relative)
{
    std::wstring path = base;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    return path + relative;
}

HANDLE OpenArchivePipe()
{
    HWND tcWindow = FindTotalCommanderWindow();
    DWORD tcPid = 0;
    if (!tcWindow || !GetWindowThreadProcessId(tcWindow, &tcPid))
        return INVALID_HANDLE_VALUE;
    const std::wstring pipeName = L"\\\\.\\pipe\\SftpArchive." + std::to_wstring(tcPid);
    return CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}

bool StartArchiveRequest(HANDLE pipe, DWORD operation, std::wstring_view sourcePath,
                         std::wstring_view targetPath, std::string_view items,
                         std::string& error)
{
    const std::string sourceUtf8 = ToUtf8(sourcePath);
    const std::string targetUtf8 = ToUtf8(targetPath);
    if ((sourcePath.size() && sourceUtf8.empty()) || (targetPath.size() && targetUtf8.empty()) ||
        sourceUtf8.size() > 64 * 1024 || targetUtf8.size() > 64 * 1024 || items.size() > 1024 * 1024) {
        error = "Could not prepare the SFTP archive request.";
        return false;
    }
    const ArchiveRequest request{ kArchiveMagic, operation,
                                  static_cast<DWORD>(sourceUtf8.size()),
                                  static_cast<DWORD>(targetUtf8.size()),
                                  static_cast<DWORD>(items.size()) };
    return WriteExact(pipe, &request, sizeof(request)) &&
           WriteExact(pipe, sourceUtf8.data(), request.sourcePathBytes) &&
           WriteExact(pipe, targetUtf8.data(), request.targetPathBytes) &&
           WriteExact(pipe, items.data(), request.itemBytes) &&
           ReadResult(pipe, error);
}

void LogSftpOperationResult(std::wstring_view sourcePath, std::wstring_view targetPath,
                            std::wstring_view action, std::wstring_view shortcut, int result)
{
    const std::wstring_view logPath = IsSftpVirtualPath(targetPath) ? targetPath
        : IsSftpVirtualPath(sourcePath) ? sourcePath : std::wstring_view{};
    if (logPath.empty())
        return;

    const wchar_t* outcome = result == 0 ? L"complete" : result == kOperationCanceled ? L"canceled" : L"failed";
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE)
        return;
    std::string error;
    const std::wstring source = DescribeOperationPath(sourcePath);
    const std::wstring target = DescribeOperationPath(targetPath);
    const std::wstring location = action == L"Delete" ? source
        : target.empty() ? source
        : source.empty() ? target
        : source + (action == L"Diff/sync" ? L" <-> " : L" -> ") + target;
    const std::wstring status = std::wstring(action) + L" " + location + L" " + outcome +
        L" (" + std::wstring(shortcut) + L")";
    StartArchiveRequest(pipe, kArchiveLogStatus, logPath, L"", ToUtf8(status), error);
    CloseHandle(pipe);
}

int FinishSftpOperation(std::wstring_view sourcePath, std::wstring_view targetPath,
                        std::wstring_view action, std::wstring_view shortcut, int result)
{
    LogSftpOperationResult(sourcePath, targetPath, action, shortcut, result);
    return result == kOperationCanceled ? 0 : result;
}

bool WriteArchiveEnd(HANDLE pipe)
{
    const DWORD endMarker = 0;
    return WriteExact(pipe, &endMarker, sizeof(endMarker));
}

bool SendFileToArchivePipe(HANDLE pipe, HANDLE file)
{
    std::array<char, kChunkSize> buffer{};
    DWORD count = 0;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count) {
        if (!WriteExact(pipe, &count, sizeof(count)) || !WriteExact(pipe, buffer.data(), count))
            return false;
    }
    return WriteArchiveEnd(pipe);
}

bool ReceiveArchivePipeToFile(HANDLE pipe, HANDLE file)
{
    std::array<char, kChunkSize> buffer{};
    for (;;) {
        DWORD length = 0;
        if (!ReadExact(pipe, &length, sizeof(length)))
            return false;
        if (length == 0)
            return true;
        if (length > buffer.size() || !ReadExact(pipe, buffer.data(), length) ||
            !WriteExact(file, buffer.data(), length))
            return false;
    }
}

std::wstring GetTarPath()
{
    std::array<wchar_t, MAX_PATH> systemDir{};
    const UINT length = GetSystemDirectoryW(systemDir.data(), static_cast<UINT>(systemDir.size()));
    if (length == 0 || length >= systemDir.size())
        return {};
    std::wstring tar(systemDir.data(), length);
    tar += L"\\tar.exe";
    return GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring{} : tar;
}

std::wstring Get7ZipPath()
{
    std::array<wchar_t, 32768> path{};
    const DWORD found = SearchPathW(nullptr, L"7z.exe", nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr);
    if (found > 0 && found < path.size())
        return std::wstring(path.data(), found);
    constexpr std::array<const wchar_t*, 2> candidates = {
        L"C:\\Program Files\\7-Zip\\7z.exe",
        L"C:\\Program Files (x86)\\7-Zip\\7z.exe",
    };
    for (const wchar_t* candidate : candidates) {
        if (GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return {};
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        std::array<wchar_t, MAX_PATH> temp{};
        if (!GetTempPathW(static_cast<DWORD>(temp.size()), temp.data()))
            return;
        std::array<wchar_t, MAX_PATH> name{};
        if (!GetTempFileNameW(temp.data(), L"SFA", 0, name.data()))
            return;
        DeleteFileW(name.data());
        if (CreateDirectoryW(name.data(), nullptr))
            path_ = name.data();
    }

    ~TemporaryDirectory()
    {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    const std::wstring& path() const noexcept { return path_; }
    explicit operator bool() const noexcept { return !path_.empty(); }

private:
    std::wstring path_;
};

bool RunHiddenProcess(const std::wstring& executable, std::wstring command, std::string& output)
{
    SECURITY_ATTRIBUTES inheritable{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &inheritable, 0))
        return false;
    SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = outputWrite;
    startup.hStdError = outputWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        CloseHandle(outputRead);
        CloseHandle(outputWrite);
        return false;
    }
    CloseHandle(outputWrite);
    std::thread drainOutput([&] {
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        while (ReadFile(outputRead, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count)
            output.append(buffer.data(), count);
        CloseHandle(outputRead);
    });
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    drainOutput.join();
    if (exitCode != 0)
        output = "7-Zip exit " + std::to_string(exitCode) + ":\n" + output;
    return exitCode == 0;
}

bool ExtractWith7Zip(const std::wstring& archive, const std::wstring& destination, std::wstring& error)
{
    const std::wstring sevenZip = Get7ZipPath();
    if (sevenZip.empty()) {
        error = L"7z.exe was not found. Install 7-Zip or add its directory to PATH.";
        return false;
    }
    const std::wstring command = L"\"" + sevenZip + L"\" x -y -bd -o\"" + destination + L"\" \"" + archive + L"\"";
    std::string output;
    // 7-Zip unwraps .tar.gz/.tgz/.tar.bz2/.tar.xz into a TAR file first.
    // Extract that one TAR layer too, so all common TAR-compressed formats
    // yield their contained files rather than an intermediate .tar file.
    std::vector<std::filesystem::path> entries;
    std::error_code fsError;
    const bool extracted = RunHiddenProcess(sevenZip, command, output);
    for (const auto& entry : std::filesystem::directory_iterator(destination, fsError))
        entries.push_back(entry.path());
    const bool hasValidatedTar = !fsError && entries.size() == 1 && entries.front().extension() == L".tar";
    bool acceptedTrailerWarning = false;
    if (!extracted && hasValidatedTar && output.find("after the end of the payload data") != std::string::npos) {
        std::string tarCheck;
        const std::wstring tarPath = GetTarPath();
        if (!tarPath.empty() && RunHiddenProcess(tarPath, L"\"" + tarPath + L"\" -tf \"" + entries.front().wstring() + L"\"", tarCheck))
            acceptedTrailerWarning = true; // The complete TAR payload is valid.
    }
    if (!extracted && !acceptedTrailerWarning) {
        if (output.size() > 1800)
            output.erase(0, output.size() - 1800);
        error = L"7-Zip could not extract the selected archive.";
        if (!output.empty())
            error += L"\n\n" + FromUtf8(output);
        return false;
    }
    if (!fsError && entries.size() == 1 && entries.front().extension() == L".tar") {
        const std::filesystem::path tarFile = entries.front();
        output.clear();
        if (!RunHiddenProcess(sevenZip, L"\"" + sevenZip + L"\" x -y -bd -o\"" + destination + L"\" \"" + tarFile.wstring() + L"\"", output)) {
            if (output.size() > 1800)
                output.erase(0, output.size() - 1800);
            error = L"7-Zip could not extract the TAR payload.";
            if (!output.empty())
                error += L"\n\n" + FromUtf8(output);
            return false;
        }
        std::filesystem::remove(tarFile, fsError);
    }
    return true;
}

bool WriteTopLevelList(const std::wstring& directory, const std::wstring& listPath)
{
    HANDLE file = CreateFileW(listPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::wstring line = entry.path().wstring() + L"\r\n";
        ok = ok && WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr) &&
             written == line.size() * sizeof(wchar_t);
        if (!ok)
            break;
    }
    CloseHandle(file);
    return ok && !error;
}

bool CopyExtractedContents(const std::wstring& source, const std::wstring& target, std::wstring& error)
{
    std::wstring destination(target);
    // TC passes "%T." to preserve a drive root in a quoted command line.
    // Remove only the trailing dot, retaining the root backslash in "Z:\\.".
    if (destination.size() >= 2 && destination.ends_with(L"\\."))
        destination.pop_back();
    if (destination.empty()) {
        error = L"The extraction target path is empty.";
        return false;
    }
    std::error_code fsError;
    const std::filesystem::path destinationPath(destination);
    if (std::filesystem::exists(destinationPath, fsError)) {
        if (fsError || !std::filesystem::is_directory(destinationPath, fsError)) {
            error = L"The extraction target is not a directory: " + destination;
            return false;
        }
    } else {
        std::filesystem::create_directories(destinationPath, fsError);
    }
    if (fsError) {
        error = L"Could not create the extraction target directory: " + destination +
                L" (Windows error " + std::to_wstring(fsError.value()) + L").";
        return false;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, fsError)) {
        if (fsError || (entry.status(fsError).permissions() == std::filesystem::perms::unknown)) {
            error = L"Could not inspect extracted archive contents.";
            return false;
        }
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = L"Archive contains an unsupported link or reparse point.";
            return false;
        }
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), source, fsError);
        if (fsError || relative.empty() || relative.native().starts_with(L"..")) {
            error = L"Archive contains an invalid path.";
            return false;
        }
        const std::filesystem::path output = destinationPath / relative;
        if (entry.is_directory(fsError)) {
            std::filesystem::create_directories(output, fsError);
        } else if (entry.is_regular_file(fsError)) {
            std::filesystem::create_directories(output.parent_path(), fsError);
            if (!fsError)
                std::filesystem::copy_file(entry.path(), output, std::filesystem::copy_options::overwrite_existing, fsError);
        } else {
            error = L"Archive contains an unsupported special file.";
            return false;
        }
        if (fsError) {
            error = L"Could not copy extracted archive contents.";
            return false;
        }
    }
    return true;
}

int StreamLocalArchiveToSftp(const std::wstring& sourcePath, const std::wstring& targetPath,
                              const std::wstring& listPath, DWORD operation = kArchivePut,
                              const std::wstring* archiveTarget = nullptr)
{
    std::vector<std::wstring> tarNames;
    if (!ReadRelativeTarNames(sourcePath, listPath, tarNames)) {
        ShowError(L"Could not prepare the selected files for archive streaming.");
        return 2;
    }
    const std::wstring remoteArchive = operation == kArchivePut
        ? archiveTarget ? *archiveTarget : JoinPath(targetPath, ArchiveName(tarNames))
        : targetPath;
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        ShowError(L"The installed SFTP plugin does not provide the archive service. Replace the WFX file and restart Total Commander.");
        return 2;
    }
    std::string remoteError;
    if (!StartArchiveRequest(pipe, operation, L"", remoteArchive, "", remoteError)) {
        CloseHandle(pipe);
        ShowError(remoteError.empty() ? L"The SFTP archive service could not start remote archive creation." : FromUtf8(remoteError));
        return 2;
    }

    const std::wstring tarPath = GetTarPath();
    if (tarPath.empty()) {
        CloseHandle(pipe);
        ShowError(L"Windows tar.exe was not found.");
        return 2;
    }
    SECURITY_ATTRIBUTES inheritable{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 0) ||
        !CreatePipe(&stderrRead, &stderrWrite, &inheritable, 0)) {
        CloseHandle(pipe);
        if (stdoutRead) CloseHandle(stdoutRead);
        if (stdoutWrite) CloseHandle(stdoutWrite);
        if (stderrRead) CloseHandle(stderrRead);
        if (stderrWrite) CloseHandle(stderrWrite);
        ShowError(L"Could not create local archive pipes.");
        return 2;
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const std::wstring sourceRoot = NormalizeLocalDirectory(sourcePath);
    std::wstring tarArguments = L" --";
    for (const std::wstring& name : tarNames) {
        tarArguments += L" " + QuoteWindowsArgument(name);
        if (tarArguments.size() > 30000) {
            CloseHandle(pipe);
            ShowError(L"Too many selected paths for Windows tar.exe. Select a parent directory instead.");
            return 2;
        }
    }
    LocalTarInvocation invocation = CreateLocalTarInvocation(tarPath, sourceRoot, L" -cf - -C ", tarArguments);
    if (invocation.executable.empty() || !CreateProcessW(invocation.executable.c_str(), invocation.command.data(), nullptr,
                                                          nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(pipe); CloseHandle(stdoutRead); CloseHandle(stdoutWrite); CloseHandle(stderrRead); CloseHandle(stderrWrite);
        ShowError(L"Could not start Windows tar.exe.");
        return 2;
    }
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    std::string tarError;
    std::thread drainStderr([&] {
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        while (ReadFile(stderrRead, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count)
            tarError.append(buffer.data(), count);
        CloseHandle(stderrRead);
    });
    std::array<char, kChunkSize> buffer{};
    DWORD count = 0;
    bool streamed = true;
    while (ReadFile(stdoutRead, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count) {
        if (!WriteExact(pipe, &count, sizeof(count)) || !WriteExact(pipe, buffer.data(), count)) {
            streamed = false;
            break;
        }
    }
    CloseHandle(stdoutRead);
    if (streamed)
        streamed = WriteArchiveEnd(pipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD tarExit = 1;
    GetExitCodeProcess(process.hProcess, &tarExit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    drainStderr.join();
    const bool remoteOk = ReadResult(pipe, remoteError);
    CloseHandle(pipe);
    if (!remoteOk) {
        ShowError(remoteError.empty() ? L"The remote archive stream ended unexpectedly." : FromUtf8(remoteError));
        return 1;
    }
    if (tarExit != 0) {
        ShowError(tarError.empty() ? L"Windows tar.exe could not create the archive stream." : FromUtf8(tarError));
        return 1;
    }
    return 0;
}

bool ReadSingleSelectedPath(const std::wstring& listPath, std::wstring& path)
{
    std::vector<std::wstring> selected;
    if (!ReadTextLines(listPath, selected) || selected.size() != 1)
        return false;
    path = selected.front();
    return true;
}

int StreamSftpArchiveToLocalFile(const std::wstring& sourceArchive, const std::wstring& localArchive)
{
    std::wstring localError;
    if (!EnsureWritableLocalFileTarget(localArchive, localError)) {
        ShowError(localError);
        return 2;
    }
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        ShowError(L"The installed SFTP plugin does not provide the archive service. Replace the WFX file and restart Total Commander.");
        return 2;
    }
    std::string error;
    if (!StartArchiveRequest(pipe, kArchiveGet, sourceArchive, L"", "", error)) {
        CloseHandle(pipe);
        ShowError(FromUtf8(error));
        return 1;
    }
    HANDLE output = CreateFileW(localArchive.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool copied = output != INVALID_HANDLE_VALUE && ReceiveArchivePipeToFile(pipe, output);
    if (output != INVALID_HANDLE_VALUE)
        CloseHandle(output);
    const bool remoteOk = copied && ReadResult(pipe, error);
    CloseHandle(pipe);
    if (!copied || !remoteOk) {
        DeleteFileW(localArchive.c_str());
        ShowError(error.empty() ? L"Could not save the remote archive locally." : FromUtf8(error));
        return 1;
    }
    return 0;
}

int StreamSftpPackToLocalFile(const std::wstring& sourcePath, std::string_view items,
                               const std::wstring& localArchive)
{
    std::wstring localError;
    if (!EnsureWritableLocalFileTarget(localArchive, localError)) {
        ShowError(localError);
        return 2;
    }
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        ShowError(L"The installed SFTP plugin does not provide the archive service. Replace the WFX file and restart Total Commander.");
        return 2;
    }
    std::string error;
    if (!StartArchiveRequest(pipe, kArchivePack, sourcePath, L"", items, error)) {
        CloseHandle(pipe);
        ShowError(FromUtf8(error));
        return 1;
    }
    HANDLE output = CreateFileW(localArchive.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool copied = output != INVALID_HANDLE_VALUE && ReceiveArchivePipeToFile(pipe, output);
    if (output != INVALID_HANDLE_VALUE)
        CloseHandle(output);
    const bool remoteOk = copied && ReadResult(pipe, error);
    CloseHandle(pipe);
    if (!copied || !remoteOk) {
        DeleteFileW(localArchive.c_str());
        ShowError(error.empty() ? L"Could not create the remote archive locally." : FromUtf8(error));
        return 1;
    }
    return 0;
}

int StreamSftpArchiveToSftp(const std::wstring& sourcePath, const std::wstring& targetPath,
                            DWORD operation, std::string_view items)
{
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        ShowError(L"The installed SFTP plugin does not provide the archive service. Replace the WFX file and restart Total Commander.");
        return 2;
    }
    std::string error;
    const bool ok = StartArchiveRequest(pipe, operation, sourcePath, targetPath, items, error);
    CloseHandle(pipe);
    if (!ok) {
        ShowError(error.empty() ? L"The remote archive operation failed." : FromUtf8(error));
        return 1;
    }
    return 0;
}

bool DeleteLocalSource(const std::wstring& sourcePath, const std::vector<std::wstring>& items)
{
    std::error_code error;
    for (const std::wstring& item : items) {
        const std::filesystem::path path = std::filesystem::path(sourcePath) / item;
        std::filesystem::remove_all(path, error);
        if (error) {
            ShowError(L"The target received the TAR stream, but a local source item could not be deleted: " + path.wstring());
            return false;
        }
    }
    return true;
}

bool DeleteSftpSource(const std::wstring& sourcePath, std::string_view items)
{
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        ShowError(L"The target received the TAR stream, but the SFTP archive service is unavailable for source cleanup.");
        return false;
    }
    std::string error;
    const bool ok = StartArchiveRequest(pipe, kArchiveDeleteRemote, sourcePath, L"", items, error);
    CloseHandle(pipe);
    if (!ok) {
        ShowError(error.empty() ? L"The target received the TAR stream, but a remote source item could not be deleted." : FromUtf8(error));
        return false;
    }
    return true;
}

int StreamSftpPackToLocalExtraction(const std::wstring& sourcePath, std::string_view items,
                                    const std::wstring& targetPath);

bool WriteLocalDiffItemList(const std::wstring& sourcePath, const std::vector<std::wstring>& items,
                            std::wstring& listPath)
{
    std::array<wchar_t, MAX_PATH> temp{};
    if (!GetTempPathW(static_cast<DWORD>(temp.size()), temp.data()) ||
        !GetTempFileNameW(temp.data(), L"SFD", 0, temp.data()))
        return false;
    listPath = temp.data();
    HANDLE file = CreateFileW(listPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    for (const std::wstring& item : items) {
        const std::wstring line = (std::filesystem::path(sourcePath) / item).wstring() + L"\r\n";
        ok = ok && WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr) &&
             written == line.size() * sizeof(wchar_t);
        if (!ok)
            break;
    }
    CloseHandle(file);
    if (!ok)
        DeleteFileW(listPath.c_str());
    return ok;
}

bool ApplyLocalDiffChanges(const std::wstring& remotePath, const std::wstring& mirror,
                           const LocalDiffChanges& changes, std::wstring& error)
{
    if (!changes.deletions.empty() && !DeleteSftpSource(remotePath, JoinArchiveItems(changes.deletions))) {
        error = L"Could not delete the remote items selected by the local diff session.";
        return false;
    }
    if (changes.uploads.empty())
        return true;
    std::wstring listPath;
    if (!WriteLocalDiffItemList(mirror, changes.uploads, listPath)) {
        error = L"Could not prepare the local diff changes for upload.";
        return false;
    }
    const int upload = StreamLocalArchiveToSftp(mirror, remotePath, listPath, kArchiveExtract);
    DeleteFileW(listPath.c_str());
    if (upload != 0) {
        error = L"Could not apply the local diff changes to the remote directory.";
        return false;
    }
    return true;
}

bool ConfirmLocalDiffApply(const LocalDiffChanges* left, const LocalDiffChanges* right)
{
    const size_t deletions = (left ? left->deletions.size() : 0) + (right ? right->deletions.size() : 0);
    const size_t uploads = (left ? left->uploads.size() : 0) + (right ? right->uploads.size() : 0);
    const std::wstring message = L"The local Synchronize Directories session changed one or more SFTP mirrors.\n\n" +
        std::to_wstring(uploads) + L" remote files or directories will be created or overwritten.\n" +
        std::to_wstring(deletions) + L" remote files or directories will be deleted.\n\n" +
        L"Apply these changes to the original SFTP directory now?";
    return ShowRouterMessage(message, L"Apply SFTP Local Diff Changes",
                             MB_YESNO | MB_ICONWARNING) == IDYES;
}

int LocalDiff(const std::wstring& sourcePath, const std::wstring& targetPath)
{
    const bool sourceSftp = IsSftpVirtualPath(sourcePath);
    const bool targetSftp = IsSftpVirtualPath(targetPath);
    if (!sourceSftp && !targetSftp) {
        ShowError(L"Alt+F12 local diff requires at least one SFTP panel.");
        return 2;
    }
    // %P is TC's active source panel. Reorder source/target to preserve the
    // physical left/right layout in the local Synchronize Directories window.
    const bool activeRight = IsRightPanelActive();
    const std::wstring& leftPath = activeRight ? targetPath : sourcePath;
    const std::wstring& rightPath = activeRight ? sourcePath : targetPath;
    const bool leftSftp = IsSftpVirtualPath(leftPath);
    const bool rightSftp = IsSftpVirtualPath(rightPath);
    const std::wstring totalCommander = GetTotalCommanderExecutable();
    if (totalCommander.empty()) {
        ShowError(L"Could not find the Total Commander executable.");
        return 2;
    }
    LocalDiffSession session;
    std::wstring error;
    if (!session.Create(error)) {
        ShowError(error);
        return 2;
    }
    const std::wstring left = leftSftp ? session.leftMirror() : TargetForPrompt(leftPath);
    const std::wstring right = rightSftp ? session.rightMirror() : TargetForPrompt(rightPath);
    if ((leftSftp && StreamSftpPackToLocalExtraction(leftPath, "", left) != 0) ||
        (rightSftp && StreamSftpPackToLocalExtraction(rightPath, "", right) != 0)) {
        session.Cleanup();
        return 1;
    }
    if ((leftSftp && !session.SnapshotLeft(error)) || (rightSftp && !session.SnapshotRight(error))) {
        ShowError(error);
        session.Cleanup();
        return 1;
    }
    if (!session.RunSynchronizeDirectories(totalCommander, left, right, error)) {
        ShowError(error);
        session.Cleanup();
        return 1;
    }
    LocalDiffChanges leftChanges;
    LocalDiffChanges rightChanges;
    if ((leftSftp && !session.CollectLeftChanges(leftChanges, error)) ||
        (rightSftp && !session.CollectRightChanges(rightChanges, error))) {
        ShowError(error + L"\n\nThe local diff session was retained for inspection:\n" + session.root());
        return 1;
    }
    const LocalDiffChanges* leftSftpChanges = leftSftp ? &leftChanges : nullptr;
    const LocalDiffChanges* rightSftpChanges = rightSftp ? &rightChanges : nullptr;
    if ((leftSftpChanges && !leftSftpChanges->empty()) || (rightSftpChanges && !rightSftpChanges->empty())) {
        if (!ConfirmLocalDiffApply(leftSftpChanges, rightSftpChanges)) {
            ShowInfo(L"Remote changes were not applied. The local diff session was retained for inspection:\n" + session.root());
            return kOperationCanceled;
        }
        if ((leftSftp && !ApplyLocalDiffChanges(leftPath, left, leftChanges, error)) ||
            (rightSftp && !ApplyLocalDiffChanges(rightPath, right, rightChanges, error))) {
            ShowError(error + L"\n\nThe local diff session was retained for inspection:\n" + session.root());
            return 1;
        }
    }
    session.Cleanup();
    return 0;
}

int StreamSftpPackToLocalExtraction(const std::wstring& sourcePath, std::string_view items,
                                    const std::wstring& targetPath)
{
    std::wstring localError;
    if (!EnsureWritableLocalDirectory(targetPath, localError)) {
        ShowError(localError);
        return 2;
    }
    const std::wstring tarPath = GetTarPath();
    if (tarPath.empty()) {
        ShowError(L"Windows tar.exe was not found.");
        return 2;
    }
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        ShowError(L"The installed SFTP plugin does not provide the archive service. Replace the WFX file and restart Total Commander.");
        return 2;
    }
    std::string error;
    if (!StartArchiveRequest(pipe, kArchivePack, sourcePath, L"", items, error)) {
        CloseHandle(pipe);
        ShowError(FromUtf8(error));
        return 1;
    }
    SECURITY_ATTRIBUTES inheritable{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite, &inheritable, 0) ||
        !CreatePipe(&stderrRead, &stderrWrite, &inheritable, 0)) {
        if (stdinRead) CloseHandle(stdinRead);
        if (stdinWrite) CloseHandle(stdinWrite);
        if (stderrRead) CloseHandle(stderrRead);
        if (stderrWrite) CloseHandle(stderrWrite);
        CloseHandle(pipe);
        ShowError(L"Could not create local TAR pipes.");
        return 2;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = stderrWrite;
    PROCESS_INFORMATION process{};
    const std::wstring targetDirectory = NormalizeLocalDirectory(targetPath);
    LocalTarInvocation invocation = CreateLocalTarInvocation(tarPath, targetDirectory, L" -xf - -C ", L"");
    if (invocation.executable.empty() || !CreateProcessW(invocation.executable.c_str(), invocation.command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        CloseHandle(pipe); CloseHandle(stdinRead); CloseHandle(stdinWrite); CloseHandle(stderrRead); CloseHandle(stderrWrite);
        ShowError(L"Could not start Windows tar.exe.");
        return 2;
    }
    CloseHandle(stdinRead);
    CloseHandle(stderrWrite);
    std::string tarError;
    std::thread drainStderr([&] {
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        while (ReadFile(stderrRead, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count)
            tarError.append(buffer.data(), count);
        CloseHandle(stderrRead);
    });
    const bool copied = ReceiveArchivePipeToFile(pipe, stdinWrite);
    CloseHandle(stdinWrite);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD tarExit = 1;
    GetExitCodeProcess(process.hProcess, &tarExit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    drainStderr.join();
    const bool remoteOk = copied && ReadResult(pipe, error);
    CloseHandle(pipe);
    if (tarExit != 0) {
        ShowError(tarError.empty() ? L"Windows tar.exe could not extract the remote TAR stream." : FromUtf8(tarError));
        return 1;
    }
    if (!remoteOk) {
        ShowError(error.empty() ? L"Could not read the remote TAR stream." : FromUtf8(error));
        return 1;
    }
    return 0;
}

bool IsRemoteDirectory(const std::wstring& path)
{
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE)
        return false;
    std::string error;
    const bool directory = StartArchiveRequest(pipe, kArchiveIsDirectory, path, L"", "", error);
    CloseHandle(pipe);
    return directory;
}

int ExtractLocalArchive(const std::wstring& archive, const std::wstring& targetPath, const std::wstring* sftpTarget)
{
    TemporaryDirectory temporary;
    if (!temporary) {
        ShowError(L"Could not create the temporary archive directory.");
        return 2;
    }
    const std::wstring extracted = JoinPath(temporary.path(), L"extracted");
    if (!CreateDirectoryW(extracted.c_str(), nullptr)) {
        ShowError(L"Could not create the temporary extraction directory.");
        return 2;
    }
    std::wstring error;
    if (!ExtractWith7Zip(archive, extracted, error)) {
        ShowError(error);
        return 1;
    }
    if (!sftpTarget)
        return CopyExtractedContents(extracted, targetPath, error) ? 0 : (ShowError(error), 1);

    const std::wstring listPath = JoinPath(temporary.path(), L"items.txt");
    if (!WriteTopLevelList(extracted, listPath)) {
        ShowError(L"Could not prepare the extracted files for transfer.");
        return 1;
    }
    return StreamLocalArchiveToSftp(extracted, *sftpTarget, listPath, kArchiveExtract);
}

int ExtractSftpArchive(const std::wstring& sourceArchive, const std::wstring& targetPath, const std::wstring* sftpTarget)
{
    TemporaryDirectory temporary;
    if (!temporary) {
        ShowError(L"Could not create the temporary archive directory.");
        return 2;
    }
    std::wstring archiveName = std::filesystem::path(sourceArchive).filename().wstring();
    if (archiveName.empty())
        archiveName = L"archive";
    const std::wstring localArchive = JoinPath(temporary.path(), archiveName);
    const int download = StreamSftpArchiveToLocalFile(sourceArchive, localArchive);
    if (download != 0)
        return download;
    return ExtractLocalArchive(localArchive, targetPath, sftpTarget);
}

std::wstring GetExecutableDirectory()
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    std::wstring directory(path.data(), length);
    const size_t separator = directory.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : directory.substr(0, separator);
}

bool SetIniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key, const wchar_t* value,
                 std::wstring& error)
{
    if (WritePrivateProfileStringW(section, key, value, path.c_str()))
        return true;
    error = L"Could not write " + std::wstring(section) + L"/" + key + L" to " + path +
            L" (Windows error " + std::to_wstring(GetLastError()) + L").";
    return false;
}

bool InitializePortableRouter()
{
    if (FindTotalCommanderWindow()) {
        ShowError(L"Close Total Commander before registering SFTP router shortcuts.");
        return false;
    }
    const std::filesystem::path routerDirectory(GetExecutableDirectory());
    const std::filesystem::path tcDirectory = routerDirectory.parent_path().parent_path().parent_path();
    const std::wstring wincmdIni = (tcDirectory / L"Wincmd.ini").wstring();
    const std::wstring userIni = (tcDirectory / L"usercmd.ini").wstring();
    if (GetFileAttributesW(wincmdIni.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ShowError(L"Could not find Wincmd.ini beside the portable Total Commander executable.");
        return false;
    }
    const wchar_t* const commands[][2] = {
        { L"em_SftpArchivePack", L"pack \"%P.\" \"%T.\" \"%L\"" },
        { L"em_SftpArchiveUnpack", L"unpack \"%P.\" \"%T.\" \"%L\"" },
        { L"em_SftpTarCopy", L"copy \"%P.\" \"%T.\" \"%L\"" },
        { L"em_SftpTarMove", L"move \"%P.\" \"%T.\" \"%L\"" },
        { L"em_SftpRemoteDelete", L"delete \"%P.\" \"%T.\" \"%L\"" },
        { L"em_SftpPrewarmManifest", L"prewarm \"%P.\"" },
        { L"em_SftpLocalDiff", L"localdiff \"%P.\" \"%T.\"" },
    };
    const wchar_t* const shortcuts[][2] = {
        { L"A+F5", L"em_SftpArchivePack" }, { L"A+F6", L"em_SftpArchiveUnpack" },
        { L"A+F7", L"em_SftpTarCopy" }, { L"A+F8", L"em_SftpTarMove" },
        { L"A+F9", L"em_SftpRemoteDelete" }, { L"A+F11", L"em_SftpPrewarmManifest" },
        { L"A+F12", L"em_SftpLocalDiff" },
    };
    constexpr wchar_t kRouterCommand[] = L"%COMMANDER_PATH%\\Plugins\\Wfx\\SFTP\\SftpArchiveRouter.exe";
    std::wstring error;
    for (const auto& command : commands) {
        if (!SetIniValue(userIni, command[0], L"cmd", kRouterCommand, error) ||
            !SetIniValue(userIni, command[0], L"param", command[1], error)) {
            ShowError(error);
            return false;
        }
    }
    for (const auto& shortcut : shortcuts) {
        if (!SetIniValue(wincmdIni, L"Shortcuts", shortcut[0], shortcut[1], error)) {
            ShowError(error);
            return false;
        }
    }
    ShowRouterMessage(L"Registered Alt+F5 through Alt+F9, Alt+F11, and Alt+F12 for SFTP archive operations. Restart Total Commander to apply them.",
                      L"SFTP Archive Router", MB_OK | MB_ICONINFORMATION);
    return true;
}

int Pack(const std::wstring& sourcePath, const std::wstring& targetPath, const std::wstring& listPath)
{
    const bool sourceSftp = IsSftpVirtualPath(sourcePath);
    const bool targetSftp = IsSftpVirtualPath(targetPath);
    if (!sourceSftp && !targetSftp)
        return RunNativeTcCommand(L"cm_PackFiles") ? 0 : 1;

    std::vector<std::wstring> items;
    if (!ReadRelativeTarNames(sourcePath, listPath, items)) {
        ShowError(L"Could not prepare the selected files for archive streaming.");
        return 2;
    }
    std::wstring archiveTarget = JoinPath(TargetForPrompt(targetPath), ArchiveName(items));
    if (!PromptForTarget(L"Archive file name:", archiveTarget))
        return kOperationCanceled;
    const std::string itemText = JoinArchiveItems(items);
    if (!sourceSftp)
        return StreamLocalArchiveToSftp(sourcePath, targetPath, listPath, kArchivePut, &archiveTarget);
    if (targetSftp)
        return StreamSftpArchiveToSftp(sourcePath, archiveTarget, kArchivePackToRemote, itemText);
    return StreamSftpPackToLocalFile(sourcePath, itemText, archiveTarget);
}

int Copy(const std::wstring& sourcePath, const std::wstring& targetPath, const std::wstring& listPath)
{
    const bool sourceSftp = IsSftpVirtualPath(sourcePath);
    const bool targetSftp = IsSftpVirtualPath(targetPath);
    if (!sourceSftp && !targetSftp)
        return RunNativeTcCommand(L"cm_Copy") ? 0 : 1;

    std::vector<std::wstring> items;
    if (!ReadRelativeTarNames(sourcePath, listPath, items)) {
        ShowError(L"Could not prepare the selected files for TAR copy.");
        return 2;
    }

    std::wstring target = TargetForPrompt(targetPath);
    if (!PromptForTarget(L"Target directory:", target))
        return kOperationCanceled;
    const std::string itemText = JoinArchiveItems(items);
    if (!sourceSftp)
        return StreamLocalArchiveToSftp(sourcePath, target, listPath, kArchiveExtract);
    if (!targetSftp)
        return StreamSftpPackToLocalExtraction(sourcePath, itemText, target);
    return StreamSftpArchiveToSftp(sourcePath, target, kArchivePackExtractRemote, itemText);
}

int Move(const std::wstring& sourcePath, const std::wstring& targetPath, const std::wstring& listPath)
{
    const bool sourceSftp = IsSftpVirtualPath(sourcePath);
    const bool targetSftp = IsSftpVirtualPath(targetPath);
    if (!sourceSftp && !targetSftp)
        return RunNativeTcCommand(L"cm_MoveOnly") ? 0 : 1;

    std::vector<std::wstring> items;
    if (!ReadRelativeTarNames(sourcePath, listPath, items)) {
        ShowError(L"Could not prepare the selected files for accelerated move.");
        return 2;
    }
    std::wstring target = TargetForPrompt(targetPath);
    if (!PromptForTarget(L"Target directory:", target))
        return kOperationCanceled;
    const std::string itemText = JoinArchiveItems(items);
    int copied = 1;
    if (!sourceSftp)
        copied = StreamLocalArchiveToSftp(sourcePath, target, listPath, kArchiveExtract);
    else if (!targetSftp)
        copied = StreamSftpPackToLocalExtraction(sourcePath, itemText, target);
    else
        copied = StreamSftpArchiveToSftp(sourcePath, target, kArchivePackExtractRemote, itemText);
    if (copied != 0)
        return copied;

    // Only delete after the target command has completed successfully.
    if (sourceSftp)
        return DeleteSftpSource(sourcePath, itemText) ? 0 : 1;
    return DeleteLocalSource(sourcePath, items) ? 0 : 1;
}

int Delete(const std::wstring& sourcePath, const std::wstring& listPath)
{
    if (!IsSftpVirtualPath(sourcePath))
        return RunNativeTcCommand(L"cm_Delete") ? 0 : 1;

    std::vector<std::wstring> items;
    if (!ReadRelativeTarNames(sourcePath, listPath, items)) {
        ShowError(L"Could not prepare the selected remote files for deletion.");
        return 2;
    }
    const std::wstring message = items.size() == 1
        ? L"Delete the selected remote item permanently?\n\n" + items.front()
        : L"Delete " + std::to_wstring(items.size()) + L" selected remote items permanently?";
    if (ShowRouterMessage(message, L"SFTP Archive Router",
                          MB_YESNO | MB_ICONWARNING) != IDYES)
        return kOperationCanceled;
    return DeleteSftpSource(sourcePath, JoinArchiveItems(items)) ? 0 : 1;
}

int Prewarm(const std::wstring& sourcePath)
{
    if (!IsSftpVirtualPath(sourcePath)) {
        ShowError(L"Alt+F11 directory prewarming is available only in an SFTP panel.");
        return 2;
    }
    HANDLE pipe = OpenArchivePipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        ShowError(L"The installed SFTP plugin does not provide directory prewarming. Replace the WFX file and restart Total Commander.");
        return 2;
    }
    std::string error;
    const bool ok = StartArchiveRequest(pipe, kArchivePrewarmManifest, sourcePath, L"", "", error);
    CloseHandle(pipe);
    if (!ok) {
        ShowError(error.empty() ? L"Could not prewarm the remote directory tree." : FromUtf8(error));
        return 1;
    }
    ShowRouterMessage(L"Remote directory metadata is ready for Total Commander's native sync and compare operations.",
                      L"SFTP Directory Prewarm", MB_OK | MB_ICONINFORMATION);
    return 0;
}

int Unpack(const std::wstring& sourcePath, const std::wstring& targetPath, const std::wstring& listPath)
{
    std::wstring selectedArchive;
    if (!ReadSingleSelectedPath(listPath, selectedArchive)) {
        if (IsSftpVirtualPath(sourcePath) || IsSftpVirtualPath(targetPath))
            ShowError(L"Select exactly one archive to extract.");
        else
            return RunNativeTcCommand(L"cm_UnpackFiles") ? 0 : 1;
        return 2;
    }
    if (IsSftpVirtualPath(sourcePath) || IsSftpVirtualPath(targetPath)) {
        std::wstring target = TargetForPrompt(targetPath);
        if (!PromptForTarget(L"Target directory:", target))
            return kOperationCanceled;
        if (IsSftpVirtualPath(sourcePath)) {
            if (IsSftpVirtualPath(target))
                return ExtractSftpArchive(selectedArchive, L"", &target);
            return ExtractSftpArchive(selectedArchive, target, nullptr);
        }
        return ExtractLocalArchive(selectedArchive, L"", &target);
    }
    return RunNativeTcCommand(L"cm_UnpackFiles") ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || (argc != 2 && argc != 3 && argc != 4 && argc != 5)) {
        ShowError(L"Usage: SftpArchiveRouter.exe init\n       SftpArchiveRouter.exe prewarm <source-path>\n       SftpArchiveRouter.exe localdiff <source-path> <target-path>\n       SftpArchiveRouter.exe copy|move|delete|pack|unpack <source-path> <target-path> <selected-list>");
        if (argv) LocalFree(argv);
        return 2;
    }

    const std::wstring operation(argv[1]);
    if (argc == 2) {
        LocalFree(argv);
        if (operation == L"init")
            return InitializePortableRouter() ? 0 : 1;
        ShowError(L"Unknown archive router command.");
        return 2;
    }
    if (argc == 3) {
        const std::wstring sourcePath(argv[2]);
        LocalFree(argv);
        if (operation == L"prewarm")
            return FinishSftpOperation(sourcePath, L"", L"Prewarm", L"Alt+F11", Prewarm(sourcePath));
        ShowError(L"Unknown archive router command.");
        return 2;
    }
    if (argc == 4) {
        const std::wstring sourcePath(argv[2]);
        const std::wstring targetPath(argv[3]);
        LocalFree(argv);
        if (operation == L"localdiff")
            return FinishSftpOperation(sourcePath, targetPath, L"Diff/sync", L"Alt+F12", LocalDiff(sourcePath, targetPath));
        ShowError(L"Unknown archive router command.");
        return 2;
    }
    const std::wstring sourcePath(argv[2]);
    const std::wstring targetPath(argv[3]);
    const std::wstring listPath(argv[4]);
    LocalFree(argv);

    if (operation == L"pack")
        return FinishSftpOperation(sourcePath, targetPath, L"Compress", L"Alt+F5", Pack(sourcePath, targetPath, listPath));
    if (operation == L"copy")
        return FinishSftpOperation(sourcePath, targetPath, L"Copy", L"Alt+F7", Copy(sourcePath, targetPath, listPath));
    if (operation == L"move")
        return FinishSftpOperation(sourcePath, targetPath, L"Move", L"Alt+F8", Move(sourcePath, targetPath, listPath));
    if (operation == L"delete")
        return FinishSftpOperation(sourcePath, targetPath, L"Delete", L"Alt+F9", Delete(sourcePath, listPath));
    if (operation == L"unpack")
        return FinishSftpOperation(sourcePath, targetPath, L"Decompress", L"Alt+F6", Unpack(sourcePath, targetPath, listPath));
    ShowError(L"Unknown archive operation.");
    return 2;
}
