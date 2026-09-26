#include "output_watcher.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace artifact {
namespace {

struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
};

// Drain an outstanding request before its buffer, OVERLAPPED or handle is freed.
struct PendingRead {
    HANDLE directory;
    OVERLAPPED& operation;
    bool pending = false;
    ~PendingRead() {
        if (pending) {
            CancelIoEx(directory, &operation);
            DWORD ignored = 0;
            GetOverlappedResult(directory, &operation, &ignored, TRUE);
        }
    }
};

DWORD open_directory_chain(std::wstring path, std::vector<Handle>& handles, bool create_missing = false) {
    for (auto& c : path) if (c == L'/') c = L'\\';
    // Deliberately exclude relative paths, UNC/device paths and drive roots.
    if (path.size() < 4 || !((path[0] >= L'A' && path[0] <= L'Z') ||
                            (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || path[2] != L'\\') return ERROR_BAD_PATHNAME;
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    if (path.size() <= 3) return ERROR_BAD_PATHNAME;

    // Validate the whole path before creating any component.
    for (std::size_t start = 3; start < path.size();) {
        std::size_t end = path.find(L'\\', start);
        if (end == std::wstring::npos) end = path.size();
        const std::wstring component = path.substr(start, end - start);
        if (component.empty() || component == L"." || component == L".." ||
            component.find_first_of(L":*?\"<>|") != std::wstring::npos ||
            component.back() == L'.' || component.back() == L' ' ||
            component.find(L'\0') != std::wstring::npos) return ERROR_BAD_PATHNAME;
        start = end + 1;
    }

    // Hold every ancestor without delete sharing so the checked chain cannot
    // be renamed/replaced while this watch is active. Never follow reparse points.
    std::size_t end = 2;
    while (true) {
        const bool final = end == path.size();
        const std::wstring prefix = L"\\\\?\\" + path.substr(0, end == 2 ? 3 : end);
        if (create_missing && end != 2 && !CreateDirectoryW(prefix.c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) return error;
        }
        Handle handle(CreateFileW(prefix.c_str(), final ? FILE_LIST_DIRECTORY : FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                                      (final ? FILE_FLAG_OVERLAPPED : 0), nullptr));
        if (handle.value == INVALID_HANDLE_VALUE) return GetLastError();
        BY_HANDLE_FILE_INFORMATION info = {};
        if (!GetFileInformationByHandle(handle.value, &info)) return GetLastError();
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_DIRECTORY;
        handles.push_back(std::move(handle));
        if (final) return ERROR_SUCCESS;

        const std::size_t start = end + 1;
        end = path.find(L'\\', start);
        if (end == std::wstring::npos) end = path.size();
    }
}

DWORD deliver(const DWORD* buffer, DWORD bytes,
              const std::function<void(const OutputChange&)>& callback) {
    const auto* data = reinterpret_cast<const unsigned char*>(buffer);
    std::size_t offset = 0;
    const std::size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    while (offset < bytes) {
        if (bytes - offset < header) return ERROR_INVALID_DATA;
        const auto* entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(data + offset);
        if (entry->FileNameLength % sizeof(wchar_t) != 0 ||
            entry->FileNameLength > bytes - offset - header) return ERROR_INVALID_DATA;
        OutputChange change;
        switch (entry->Action) {
        case FILE_ACTION_ADDED: change.kind = ChangeKind::created; break;
        case FILE_ACTION_MODIFIED: change.kind = ChangeKind::modified; break;
        case FILE_ACTION_REMOVED: change.kind = ChangeKind::removed; break;
        case FILE_ACTION_RENAMED_OLD_NAME: change.kind = ChangeKind::renamed_old; break;
        case FILE_ACTION_RENAMED_NEW_NAME: change.kind = ChangeKind::renamed_new; break;
        default: return ERROR_INVALID_DATA;
        }
        change.name.assign(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
        callback(change);
        if (!entry->NextEntryOffset) return ERROR_SUCCESS;
        if (entry->NextEntryOffset < header + entry->FileNameLength ||
            entry->NextEntryOffset % sizeof(DWORD) != 0 ||
            entry->NextEntryOffset >= bytes - offset) return ERROR_INVALID_DATA;
        offset += entry->NextEntryOffset;
    }
    return ERROR_INVALID_DATA;
}

} // namespace

DWORD prepare_output_directory(const std::wstring& path) {
    std::vector<Handle> directories;
    return open_directory_chain(path, directories, true);
}

DWORD watch_output(const std::wstring& path, HANDLE stop_event,
                   const std::function<void(const OutputChange&)>& on_change) {
    if (!stop_event || stop_event == INVALID_HANDLE_VALUE || !on_change)
        return ERROR_INVALID_PARAMETER;
    std::vector<Handle> directories;
    DWORD result = open_directory_chain(path, directories);
    if (result != ERROR_SUCCESS) return result;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.value) return GetLastError();
    DWORD buffer[16384] = {};
    OVERLAPPED operation = {};
    operation.hEvent = event.value;
    PendingRead read{directories.back().value, operation};
    bool ready = false;
    for (;;) {
        DWORD state = WaitForSingleObject(stop_event, 0);
        if (state == WAIT_OBJECT_0) return ERROR_SUCCESS;
        if (state == WAIT_FAILED) return GetLastError();
        if (!ResetEvent(event.value)) return GetLastError();
        if (!ReadDirectoryChangesW(read.directory, buffer, sizeof(buffer), TRUE,
                                   FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                       FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                   nullptr, &operation, nullptr)) return GetLastError();
        read.pending = true;
        if (!ready) {
            on_change(OutputChange{ChangeKind::ready, L""});
            ready = true;
        }
        HANDLE waits[] = {stop_event, event.value};
        state = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (state == WAIT_OBJECT_0) return ERROR_SUCCESS;
        if (state == WAIT_FAILED) return GetLastError();
        if (state != WAIT_OBJECT_0 + 1) return ERROR_GEN_FAILURE;
        DWORD bytes = 0;
        const BOOL completed = GetOverlappedResult(read.directory, &operation, &bytes, FALSE);
        const DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        if (completed || error != ERROR_IO_INCOMPLETE) read.pending = false;
        if (!completed) return error;
        // An overflow means events were lost. Fail visibly; never claim healthy.
        if (!bytes) return ERROR_NOTIFY_ENUM_DIR;
        result = deliver(buffer, bytes, on_change);
        if (result != ERROR_SUCCESS) return result;
    }
}

} // namespace artifact
