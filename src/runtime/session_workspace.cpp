#include "session_workspace.h"
#include <windows.h>
#include <stdexcept>

namespace runner::runtime {
namespace fs = std::filesystem;
SessionWorkspace::~SessionWorkspace() {
    for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) CloseHandle(*it);
}
bool SessionWorkspace::valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}
void SessionWorkspace::pin_directory(const fs::path& path) {
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot pin workspace directory");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(h);
        throw std::runtime_error("workspace reparse point or non-directory rejected");
    }
    try { handles_.push_back(h); } catch (...) { CloseHandle(h); throw; }
}
const WorkspacePaths& SessionWorkspace::create(const fs::path& base,
        const std::string& session, const std::string& runtime, unsigned long long generation) {
    if (!handles_.empty() || !paths_.root.empty()) throw std::logic_error("workspace is single-use");
    const auto native = base.native();
    if (!valid_id(session) || !valid_id(runtime) || !generation || !base.is_absolute() ||
        native.size() < 3 || native[1] != L':' || native.find(L':', 2) != std::wstring::npos ||
        native.find(L'\0') != std::wstring::npos || base != base.lexically_normal())
        throw std::invalid_argument("invalid workspace identity or non-local base path");
    if (GetDriveTypeW(base.root_path().c_str()) != DRIVE_FIXED)
        throw std::invalid_argument("workspace requires a local fixed Guest volume");
    fs::path current = base.root_path();
    pin_directory(current);
    for (const auto& component : base.relative_path()) {
        if (component == L".." || component == L".") throw std::invalid_argument("workspace traversal");
        current /= component;
        // The base must be provisioned by trusted startup code, never by an Action.
        pin_directory(current);
    }
    for (const auto& name : {"session-" + session, "runtime-" + runtime}) {
        current /= name;
        if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("cannot create workspace container");
        pin_directory(current);
    }
    current /= "generation-" + std::to_string(generation);
    // An existing generation may contain actions executed by a crashed Runner.
    // Never silently reset the in-memory dedup ledger in that generation.
    if (!CreateDirectoryW(current.c_str(), nullptr))
        throw std::runtime_error("workspace generation exists or cannot be created; request a new generation");
    pin_directory(current);
    paths_.root = current;
    for (const auto* name : {"temp", "downloads", "workspace", "output", "logs"}) {
        const auto child = current / name;
        if (!CreateDirectoryW(child.c_str(), nullptr)) throw std::runtime_error("cannot create workspace child");
        pin_directory(child);
    }
    paths_.temp = current / "temp";
    paths_.downloads = current / "downloads";
    paths_.workspace = current / "workspace";
    paths_.output = current / "output";
    paths_.logs = current / "logs";
    return paths_;
}
} // namespace runner::runtime
