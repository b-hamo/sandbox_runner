#pragma once
#include "file_stability.h"
#include <memory>

namespace artifact {
struct SnapshotFile {
    std::wstring relative_path;
    std::string event_id;
    std::string sha256;
    VerifiedFile source;
};
// Why the last CAPTURE was refused. Diagnostics only: the whole capture is still
// rejected, and none of these fields loosens a check or authorizes a transfer.
struct CaptureFailure {
    DWORD error = ERROR_SUCCESS;
    const char* reason = nullptr;   // stable code, e.g. STREAMS_NOT_SUPPORTED
    std::wstring relative_path;     // empty when not tied to one Output entry
    std::wstring stream;            // first named stream, if one was found
};
// Bounded UTF-8 `problem` text for OUTPUT_SNAPSHOT_RESULT. Its first token is
// still the former problem code (OUTPUT_UNAVAILABLE or OUTPUT_TOO_LARGE).
std::string capture_problem(const CaptureFailure& failure);
// A bounded, locked manifest for pre-shutdown transfers. It is not a safety
// verdict. Only the Host scanner and final user approval can publish a file.
class OutputSnapshot {
public:
    explicit OutputSnapshot(std::wstring root);
    ~OutputSnapshot();
    OutputSnapshot(const OutputSnapshot&)=delete;
    OutputSnapshot& operator=(const OutputSnapshot&)=delete;
    DWORD capture(std::uint64_t max_file_bytes, std::uint64_t max_total_bytes);
    DWORD validate() const;
    const std::vector<SnapshotFile>& files() const { return files_; }
    // Set when capture() returns an error; reset by the next capture().
    const CaptureFailure& failure() const { return failure_; }
private:
    std::wstring root_;
    std::vector<HANDLE> directories_;
    std::vector<SnapshotFile> files_;
    CaptureFailure failure_;
    void clear() noexcept;
    DWORD fail(DWORD error, const std::wstring& relative_path, const char* reason = nullptr);
};
}
