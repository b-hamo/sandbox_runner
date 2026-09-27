#include "file_stability.h"
#include <vector>
#include <algorithm>

namespace artifact {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle&& h) noexcept : value(h.value) { h.value = INVALID_HANDLE_VALUE; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
bool valid_component(const std::wstring& p) {
    if (p.empty() || p == L"." || p == L".." || p.back() == L'.' || p.back() == L' ') return false;
    for (wchar_t c : p) if (c < 32 || c == 127 || std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos) return false;
    return true;
}
DWORD open_chain(std::wstring absolute, std::vector<Handle>& handles, bool file) {
    std::replace(absolute.begin(), absolute.end(), L'/', L'\\');
    if (absolute.size() < 4 || absolute[1] != L':' || absolute[2] != L'\\' ||
        !((absolute[0] >= L'A' && absolute[0] <= L'Z') || (absolute[0] >= L'a' && absolute[0] <= L'z')))
        return ERROR_BAD_PATHNAME;
    if (!valid_relative_file(absolute.substr(3))) return ERROR_BAD_PATHNAME;
    std::size_t end = 2;
    for (;;) {
        const bool last = end == absolute.size();
        const std::wstring prefix = L"\\\\?\\" + absolute.substr(0, end == 2 ? 3 : end);
        Handle h(CreateFileW(prefix.c_str(), last && file ? GENERIC_READ : FILE_READ_ATTRIBUTES,
                             last && file ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (h.value == INVALID_HANDLE_VALUE) return GetLastError();
        BY_HANDLE_FILE_INFORMATION info = {};
        if (!GetFileInformationByHandle(h.value, &info)) return GetLastError();
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
        if (last && file) {
            if (GetFileType(h.value) != FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
            // Do not accept aliases of data which may also be reachable outside Output.
            if (info.nNumberOfLinks != 1) return ERROR_ACCESS_DENIED;
        } else if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
        handles.push_back(std::move(h));
        if (last) return ERROR_SUCCESS;
        end = absolute.find(L'\\', end + 1);
        if (end == std::wstring::npos) end = absolute.size();
    }
}
bool same_info(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
           a.nFileIndexLow == b.nFileIndexLow && a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
           CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0 && CompareFileTime(&a.ftCreationTime, &b.ftCreationTime) == 0;
}
DWORD check_streams(const std::wstring& path) {
    WIN32_FIND_STREAM_DATA stream = {};
    HANDLE search = FindFirstStreamW((L"\\\\?\\" + path).c_str(), FindStreamInfoStandard, &stream, 0);
    if (search == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error = ERROR_SUCCESS;
    do {
        if (std::wstring(stream.cStreamName) != L"::$DATA") { error = ERROR_NOT_SUPPORTED; break; }
    } while (FindNextStreamW(search, &stream));
    if (!error && GetLastError() != ERROR_HANDLE_EOF) error = GetLastError();
    FindClose(search);
    return error;
}
}
bool valid_relative_file(const std::wstring& path) {
    if (path.empty() || path.back() == L'\\') return false;
    std::size_t start = 0;
    while (start < path.size()) {
        auto end = path.find(L'\\', start);
        if (end == std::wstring::npos) end = path.size();
        if (!valid_component(path.substr(start, end - start))) return false;
        start = end + 1;
    }
    return true;
}
DWORD observe_file(const std::wstring& root, const std::wstring& relative, FileObservation& result) {
    if (!valid_relative_file(relative)) return ERROR_BAD_PATHNAME;
    std::wstring base = root;
    std::replace(base.begin(), base.end(), L'/', L'\\');
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    const std::wstring path = base + L"\\" + relative;
    std::vector<Handle> source;
    const DWORD error = open_chain(path, source, true);
    if (error) return error;
    const DWORD streams = check_streams(path);
    if (streams) return streams;
    if (!GetFileInformationByHandle(source.back().value, &result.info)) return GetLastError();
    return ERROR_SUCCESS;
}
bool same_observation(const FileObservation& a, const FileObservation& b) {
    return same_info(a.info, b.info);
}
} // namespace artifact
