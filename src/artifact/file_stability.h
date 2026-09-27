#pragma once
#include <windows.h>
#include <string>

namespace artifact {
bool valid_relative_file(const std::wstring& path);
// Metadata is only an observation for stabilization, never Host size/hash validation.
struct FileObservation { BY_HANDLE_FILE_INFORMATION info{}; };
// Reuses the original path-chain validation and temporarily denies writers/deletion.
// No content read, hash, temporary copy, MIME detection or malware scan.
DWORD observe_file(const std::wstring& root, const std::wstring& relative, FileObservation& result);
bool same_observation(const FileObservation& a, const FileObservation& b);
} // namespace artifact
