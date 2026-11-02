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
private:
    std::wstring root_;
    std::vector<HANDLE> directories_;
    std::vector<SnapshotFile> files_;
    void clear() noexcept;
};
}
