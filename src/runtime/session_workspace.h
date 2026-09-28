#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace runner::runtime {
struct WorkspacePaths {
    std::filesystem::path root, temp, downloads, workspace, output, logs;
};

// Private Guest storage only. No general file read/write or Host export API.
// Ancestor handles exclude FILE_SHARE_DELETE for the object's lifetime.
class SessionWorkspace {
public:
    SessionWorkspace() = default;
    ~SessionWorkspace();
    SessionWorkspace(const SessionWorkspace&) = delete;
    SessionWorkspace& operator=(const SessionWorkspace&) = delete;
    const WorkspacePaths& create(const std::filesystem::path& trusted_guest_base,
                                 const std::string& session, const std::string& runtime,
                                 unsigned long long generation);
    const WorkspacePaths& paths() const { return paths_; }
    static bool valid_id(const std::string& id);
private:
    void pin_directory(const std::filesystem::path& path);
    WorkspacePaths paths_;
    std::vector<void*> handles_;
};
} // namespace runner::runtime
