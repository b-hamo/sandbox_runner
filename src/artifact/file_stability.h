#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace artifact {
bool valid_relative_file(const std::wstring& path);
// Metadata is only an observation for stabilization, never Host size/hash validation.
struct FileObservation { BY_HANDLE_FILE_INFORMATION info{}; };
// Retains the verified source and every directory in its path until transfer
// completes. The file handle is borrowed: callers must not close/reopen it.
// Metadata and the locked source are not a file-safety verdict or a snapshot.
class VerifiedFile {
public:
    VerifiedFile() = default;
    ~VerifiedFile();
    VerifiedFile(VerifiedFile&&) noexcept;
    VerifiedFile& operator=(VerifiedFile&&) noexcept;
    VerifiedFile(const VerifiedFile&) = delete;
    VerifiedFile& operator=(const VerifiedFile&) = delete;
    explicit operator bool() const noexcept { return !handles_.empty(); }
    HANDLE handle() const noexcept;
    std::uint64_t size() const noexcept;
    const FileObservation& observation() const noexcept { return observation_; }
    DWORD validate_unchanged() const;
    void reset() noexcept;
private:
    std::vector<HANDLE> handles_;
    FileObservation observation_{};
    friend DWORD open_verified_file(const std::wstring&, const std::wstring&,
                                   const FileObservation&, VerifiedFile&);
};
// On failure result is empty. Opens only an in-scope regular file, rejects
// links/alternate streams, and compares identity/size/times with stabilization.
// The file pointer starts at zero. No path-based reopen is needed for streaming.
DWORD open_verified_file(const std::wstring& root, const std::wstring& relative,
                         const FileObservation& expected, VerifiedFile& result);
// Reuses the original path-chain validation and temporarily denies writers/deletion.
// No content read, hash, temporary copy, MIME detection or malware scan.
DWORD observe_file(const std::wstring& root, const std::wstring& relative, FileObservation& result);
bool same_observation(const FileObservation& a, const FileObservation& b);
} // namespace artifact
