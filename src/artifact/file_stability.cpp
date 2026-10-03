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
DWORD open_chain(std::wstring absolute, std::vector<Handle>& handles, bool file,
                 bool lock_directories = false) {
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
                             last && file ? FILE_SHARE_READ :
                                 (lock_directories ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE),
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
DWORD check_streams(HANDLE file) {
    // Query the locked file itself. A path query could inspect a replacement
    // instead of the source whose bytes will be uploaded. One unnamed stream
    // fits this buffer; an overflow is rejected rather than enumerated broadly.
    alignas(FILE_STREAM_INFO) unsigned char buffer[1024]{};
    if (!GetFileInformationByHandleEx(file, FileStreamInfo, buffer, sizeof buffer))
        return GetLastError();
    const auto* stream = reinterpret_cast<const FILE_STREAM_INFO*>(buffer);
    const std::wstring unnamed = L"::$DATA";
    if (stream->NextEntryOffset || stream->StreamNameLength != unnamed.size() * sizeof(wchar_t) ||
        std::wstring(stream->StreamName, stream->StreamNameLength / sizeof(wchar_t)) != unnamed)
        return ERROR_NOT_SUPPORTED;
    return ERROR_SUCCESS;
}
DWORD source_path(const std::wstring& root, const std::wstring& relative, std::wstring& path) {
    if (!valid_relative_file(relative)) return ERROR_BAD_PATHNAME;
    std::wstring base = root;
    std::replace(base.begin(), base.end(), L'/', L'\\');
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    path = base + L"\\" + relative;
    if (path.size() > 32760) return ERROR_FILENAME_EXCED_RANGE;
    return ERROR_SUCCESS;
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
    std::wstring path;
    const DWORD path_error = source_path(root, relative, path);
    if (path_error) return path_error;
    std::vector<Handle> source;
    const DWORD error = open_chain(path, source, true);
    if (error) return error;
    const DWORD streams = check_streams(source.back().value);
    if (streams) return streams;
    if (!GetFileInformationByHandle(source.back().value, &result.info)) return GetLastError();
    return ERROR_SUCCESS;
}
bool same_observation(const FileObservation& a, const FileObservation& b) {
    return same_info(a.info, b.info);
}
VerifiedFile::~VerifiedFile() { reset(); }
VerifiedFile::VerifiedFile(VerifiedFile&& other) noexcept : observation_(other.observation_) {
    handles_.swap(other.handles_);
    other.observation_ = {};
}
VerifiedFile& VerifiedFile::operator=(VerifiedFile&& other) noexcept {
    if (this != &other) {
        reset();
        handles_.swap(other.handles_);
        observation_ = other.observation_;
        other.observation_ = {};
    }
    return *this;
}
void VerifiedFile::reset() noexcept {
    for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) CloseHandle(*it);
    handles_.clear();
    observation_ = {};
}
HANDLE VerifiedFile::handle() const noexcept {
    return handles_.empty() ? INVALID_HANDLE_VALUE : handles_.back();
}
std::uint64_t VerifiedFile::size() const noexcept {
    return (std::uint64_t(observation_.info.nFileSizeHigh) << 32) | observation_.info.nFileSizeLow;
}
DWORD VerifiedFile::validate_unchanged() const {
    if (handles_.empty()) return ERROR_INVALID_HANDLE;
    for (std::size_t i = 0; i < handles_.size(); ++i) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handles_[i], &info)) return GetLastError();
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
        if (i + 1 < handles_.size()) {
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
        } else {
            if (GetFileType(handles_[i]) != FILE_TYPE_DISK ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
            if (info.nNumberOfLinks != 1) return ERROR_ACCESS_DENIED;
            if (!same_info(observation_.info, info)) return ERROR_FILE_INVALID;
        }
    }
    return check_streams(handle());
}
DWORD open_verified_file(const std::wstring& root, const std::wstring& relative,
                         const FileObservation& expected, VerifiedFile& result) {
    result.reset();
    std::wstring path;
    const DWORD path_error = source_path(root, relative, path);
    if (path_error) return path_error;
    std::vector<Handle> source;
    const DWORD error = open_chain(path, source, true, true);
    if (error) return error;
    const DWORD streams = check_streams(source.back().value);
    if (streams) return streams;
    FileObservation current{};
    if (!GetFileInformationByHandle(source.back().value, &current.info)) return GetLastError();
    if (current.info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY) ||
        current.info.nNumberOfLinks != 1) return ERROR_ACCESS_DENIED;
    if (!same_observation(expected, current)) return ERROR_FILE_INVALID;
    // Reserve before ownership transfer, so allocation failure still closes all
    // already-opened source handles through their original owners.
    result.handles_.reserve(source.size());
    for (auto& h : source) {
        result.handles_.push_back(h.value);
        h.value = INVALID_HANDLE_VALUE;
    }
    result.observation_ = current;
    return ERROR_SUCCESS;
}
} // namespace artifact
