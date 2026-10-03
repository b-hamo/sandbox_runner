#include "candidate_detector.h"
#include "file_stability.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <stdexcept>
#include <algorithm>

namespace artifact {
const char* candidate_state_name(CandidateState s) {
    switch (s) {
    case CandidateState::pending: return "PENDING";
    case CandidateState::stabilizing: return "STABILIZING";
    case CandidateState::candidate: return "ARTIFACT_CANDIDATE";
    case CandidateState::unavailable: return "UNAVAILABLE";
    }
    return "UNAVAILABLE";
}
namespace {
struct PathLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
bool descendant(const std::wstring& parent, const std::wstring& child) {
    return child.size() > parent.size() && child[parent.size()] == L'\\' &&
        CompareStringOrdinal(parent.data(), static_cast<int>(parent.size()), child.data(), static_cast<int>(parent.size()), TRUE) == CSTR_EQUAL;
}
}
struct CandidateDetector::Impl {
    using Clock = std::chrono::steady_clock;
    struct Entry {
        CandidateStatus status;
        Clock::time_point due, deadline;
        FileObservation previous{};
        bool sampled = false;
    };
    std::wstring output;
    StatusSink sink;
    std::function<void(const OutputChange&)> event_sink;
    CandidateOptions options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<OutputChange> inbox;
    std::map<std::wstring, Entry, PathLess> files;
    unsigned long long revision = 0;
    bool stopping = false, healthy = true, invalidated = false;
    DWORD lost_error = ERROR_SUCCESS;
    std::thread worker;

    Impl(std::wstring root, StatusSink status,
         std::function<void(const OutputChange&)> events, CandidateOptions opts)
        : output(std::move(root)), sink(std::move(status)),
          event_sink(std::move(events)), options(opts) {
        if (!options.max_events || !options.max_files || options.debounce.count() <= 0 || options.stabilization_timeout <= options.debounce)
            throw std::invalid_argument("Invalid candidate detection options");
        worker = std::thread([this] { run(); });
    }
    ~Impl() { stop(); }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }
    void lose(DWORD error) { healthy = false; lost_error = error; }
    void unavailable(Entry& entry, DWORD error, const char* reason, std::vector<CandidateStatus>& notices) {
        entry.status.generation = ++revision;
        entry.sampled = false;
        entry.status.state = CandidateState::unavailable;
        entry.status.has_observation = false;
        entry.status.error = error;
        entry.status.detail = reason;
        notices.push_back(entry.status);
    }
    void drain(std::vector<OutputChange>& observed, std::vector<CandidateStatus>& notices) {
        while (!inbox.empty() && healthy && !stopping) {
            OutputChange event = std::move(inbox.front()); inbox.pop_front();
            observed.push_back(event);
            if (event.kind == ChangeKind::ready) continue;
            if (!valid_relative_file(event.name)) { lose(ERROR_BAD_PATHNAME); break; }
            for (auto& pair : files) {
                if (event.kind != ChangeKind::modified && descendant(event.name, pair.first))
                    unavailable(pair.second, ERROR_FILE_INVALID, "ancestor changed; previous result invalidated", notices);
            }
            auto found = files.find(event.name);
            if (found == files.end()) {
                if (files.size() >= options.max_files) { lose(ERROR_NOT_ENOUGH_MEMORY); break; }
                Entry entry;
                entry.status.path = event.name;
                found = files.emplace(event.name, std::move(entry)).first;
            }
            Entry& entry = found->second;
            if (event.kind == ChangeKind::removed || event.kind == ChangeKind::renamed_old) {
                unavailable(entry, ERROR_FILE_NOT_FOUND, "removed or renamed; previous result invalidated", notices);
            } else {
                const bool already_pending = entry.status.state == CandidateState::pending || entry.status.state == CandidateState::stabilizing;
                if (!already_pending || entry.deadline == Clock::time_point{})
                    entry.deadline = Clock::now() + options.stabilization_timeout;
                entry.sampled = false;
                entry.status.generation = ++revision;
                entry.status.state = CandidateState::pending;
                entry.status.has_observation = false;
                entry.status.error = ERROR_SUCCESS;
                entry.status.detail = "waiting for last-change debounce";
                entry.due = std::min(Clock::now() + options.debounce, entry.deadline);
                notices.push_back(entry.status);
            }
        }
        if ((!healthy || stopping) && !invalidated) {
            inbox.clear();
            for (auto& pair : files) unavailable(pair.second, healthy ? ERROR_CANCELLED : lost_error,
                healthy ? "Runner stopped" : "watch degraded; no results remain current", notices);
            CandidateStatus global;
            global.state = CandidateState::unavailable;
            global.error = healthy ? ERROR_CANCELLED : lost_error;
            global.detail = healthy ? "candidate detection stopped" : "watch degraded; restart required; no full-tree rescan performed";
            notices.push_back(global);
            invalidated = true;
        }
    }
    void emit(const std::vector<OutputChange>& events, const std::vector<CandidateStatus>& notices) {
        for (const auto& e : events) if (event_sink) event_sink(e);
        for (const auto& n : notices) if (sink) sink(n);
    }
    void run() noexcept {
        try {
            for (;;) {
                std::vector<OutputChange> events;
                std::vector<CandidateStatus> notices;
                std::unique_lock<std::mutex> lock(mutex);
                drain(events, notices);
                lock.unlock(); emit(events, notices); lock.lock();
                if (stopping) return;
                if (!inbox.empty()) continue;
                auto selected = files.end();
                if (healthy) for (auto it = files.begin(); it != files.end(); ++it) {
                    if ((it->second.status.state == CandidateState::pending || it->second.status.state == CandidateState::stabilizing) &&
                        (selected == files.end() || it->second.due < selected->second.due)) selected = it;
                }
                if (selected == files.end()) {
                    wake.wait(lock, [this] { return stopping || !inbox.empty() || (!healthy && !invalidated); });
                    continue;
                }
                if (selected->second.due > Clock::now()) {
                    wake.wait_until(lock, selected->second.due);
                    continue;
                }
                CandidateStatus job = selected->second.status;
                job.state = CandidateState::stabilizing;
                selected->second.status.state = job.state;
                lock.unlock();
                FileObservation observation{};
                const DWORD error = observe_file(output, job.path, observation);
                events.clear(); notices.clear(); lock.lock();
                // Preserve the existing generation check: queued mutations win over observations.
                drain(events, notices);
                auto current = files.find(job.path);
                if (healthy && !stopping && current != files.end() && current->second.status.generation == job.generation) {
                    Entry& entry = current->second;
                    const bool retryable = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
                    if (Clock::now() >= entry.deadline) {
                        unavailable(entry, ERROR_TIMEOUT, "file did not stabilize before deadline", notices);
                    } else if (error && !retryable) {
                        unavailable(entry, error, "not an accessible in-scope regular file", notices);
                    } else if (!error && entry.sampled && same_observation(entry.previous, observation)) {
                        entry.status.state = CandidateState::candidate;
                        entry.status.observation = observation;
                        entry.status.has_observation = true;
                        entry.status.error = ERROR_SUCCESS;
                        entry.status.detail = "stable observations; candidate only, Host Quarantine verification required";
                        notices.push_back(entry.status);
                    } else {
                        entry.sampled = !error;
                        entry.previous = observation;
                        entry.due = std::min(Clock::now() + options.debounce, entry.deadline);
                        entry.status.state = CandidateState::stabilizing;
                        entry.status.has_observation = false;
                        entry.status.error = error;
                        entry.status.detail = error ? "writer still active; retrying" : "waiting for matching file observation";
                        notices.push_back(entry.status);
                    }
                }
                lock.unlock(); emit(events, notices);
            }
        } catch (...) {
            { std::lock_guard<std::mutex> lock(mutex); lose(ERROR_GEN_FAILURE); }
            try {
                CandidateStatus failure;
                failure.state = CandidateState::unavailable;
                failure.error = ERROR_GEN_FAILURE;
                failure.detail = "candidate detection worker failed; all cached results invalid; restart required";
                if (sink) sink(failure);
            } catch (...) {}
            // Queries fail closed even when a caller-supplied notification sink throws.
        }
    }
};
CandidateDetector::CandidateDetector(std::wstring output, StatusSink status,
                         std::function<void(const OutputChange&)> events, CandidateOptions options)
    : impl_(new Impl(std::move(output), std::move(status), std::move(events), options)) {}
CandidateDetector::~CandidateDetector() = default;
void CandidateDetector::submit(const OutputChange& event) {
    { std::lock_guard<std::mutex> lock(impl_->mutex);
      if (!impl_->healthy || impl_->stopping) return;
      if (impl_->inbox.size() >= impl_->options.max_events || event.name.size() > 32760) impl_->lose(ERROR_NOTIFY_ENUM_DIR);
      else impl_->inbox.push_back(event);
    }
    impl_->wake.notify_one();
}
void CandidateDetector::invalidate(DWORD error) {
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->lose(error ? error : ERROR_NOTIFY_ENUM_DIR); }
    impl_->wake.notify_one();
}
void CandidateDetector::stop() { impl_->stop(); }
bool CandidateDetector::get_status(const std::wstring& path, CandidateStatus& result) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->files.find(path);
    if (it == impl_->files.end()) return false;
    result = it->second.status;
    // Conservatively suppress completed results while any newer notifications await processing.
    if (!impl_->healthy || impl_->stopping || !impl_->inbox.empty()) {
        result.state = CandidateState::unavailable;
        result.has_observation = false;
        result.detail = "new events, stop or degraded watch; cached result is not current";
    }
    return true;
}
} // namespace artifact
