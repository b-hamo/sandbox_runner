#pragma once

#include "output_watcher.h"
#include "file_stability.h"
#include <chrono>
#include <memory>

namespace artifact {
enum class CandidateState { pending, stabilizing, candidate, unavailable };
struct CandidateStatus {
    std::wstring path;
    unsigned long long generation = 0;
    CandidateState state = CandidateState::pending;
    DWORD error = ERROR_SUCCESS;
    std::string detail;
    FileObservation observation{};
    bool has_observation = false; // Valid only for the current candidate generation.
};
struct CandidateOptions {
    std::chrono::milliseconds debounce{300};
    std::size_t max_events = 4096;
    std::size_t max_files = 4096;
    std::chrono::milliseconds stabilization_timeout{30000};
};
// Candidate status is a local event, not a safety verdict or SCRP wire schema.
// Sink runs on the worker; it must return promptly and must not call stop().
using StatusSink = std::function<void(const CandidateStatus&)>;

class CandidateDetector {
public:
    CandidateDetector(std::wstring output, StatusSink status,
                 std::function<void(const OutputChange&)> events = {},
                 CandidateOptions options = {});
    ~CandidateDetector();
    CandidateDetector(const CandidateDetector&) = delete;
    CandidateDetector& operator=(const CandidateDetector&) = delete;
    void submit(const OutputChange& change); // Enqueue only; no filesystem/log I/O.
    void invalidate(DWORD error); // Terminal watch degradation; requires a new session/watch.
    void stop(); // Wakes and joins the owned worker.
    bool get_status(const std::wstring& path, CandidateStatus& result) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
const char* candidate_state_name(CandidateState state);
} // namespace artifact
