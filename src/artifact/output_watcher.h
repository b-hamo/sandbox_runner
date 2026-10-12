#pragma once

#include <windows.h>
#include <functional>
#include <string>

namespace artifact {

enum class ChangeKind { ready, created, modified, removed, renamed_old, renamed_new };

struct OutputChange {
    ChangeKind kind;
    std::wstring name; // Output-relative path, including subdirectories; untrusted metadata.
};

// Create missing directories without following reparse points. Existing files
// are preserved. Uses the same local absolute-path validation as watch_output.
DWORD prepare_output_directory(const std::wstring& path);

// Blocks on the calling thread until stop_event is signalled or an error occurs.
// The caller supplies an approved absolute local Output path and owns stop_event.
// Watches the full directory subtree. Reparse targets are not traversed.
// Callbacks run on this thread and must be quick.
// Notifications are hints, not write-completion or safety decisions.
// Returns ERROR_SUCCESS on cancellation, otherwise a Win32 error code.
DWORD watch_output(const std::wstring& path, HANDLE stop_event,
                   const std::function<void(const OutputChange&)>& on_change);

} // namespace artifact
