#pragma once
#include "candidate_telemetry.h"
#include "protocol/scrp/artifact_export.h"
#include <condition_variable>
#include <deque>
#include <optional>
#include <set>
#include <thread>

namespace runner {
// One session/Control connection, one upload worker, one completion slot. Only
// poll() hands results to the Control sender; the worker never writes to WSS.
// Construct after Telemetry CHANNEL_ACK. Stop/join before detector destruction.
class ArtifactExportSession {
public:
    using Upload = std::function<scrp::ArtifactTransferResult(
        const scrp::ArtifactUploadRequest&, artifact::VerifiedFile&,
        const std::atomic<bool>&)>;
    using CurrentCandidate = std::function<bool(const std::wstring&,
        artifact::CandidateStatus&)>;
    ArtifactExportSession(std::wstring output, scrp::SessionContext session,
        std::string connection_id, Upload, CurrentCandidate,
        std::size_t max_candidates = 4096, std::size_t max_upload_ids = 4096);
    ~ArtifactExportSession();
    ArtifactExportSession(const ArtifactExportSession&) = delete;
    ArtifactExportSession& operator=(const ArtifactExportSession&) = delete;
    // CandidateTelemetry's optional observer. Thread safe, no filesystem I/O.
    void observe(const CandidateObservation&, const artifact::CandidateStatus&,
        bool current) noexcept;
    // A degraded channel is terminal for this export session; true cannot
    // restore old candidates or grants. Called by the owner on Telemetry loss.
    void set_healthy(bool healthy) noexcept;
    ::control::Replies handle(const scrp::Envelope&);
    std::vector<::control::DeferredReply> poll();
    void block() noexcept; // no join/I/O; cancels active/pending work
    void stop(); // owner/stopper thread only; block + join
    bool stopped() const noexcept { return stopped_.load(); }
private:
    struct Candidate {
        std::wstring path;
        unsigned long long generation = 0;
        artifact::FileObservation observation{};
    };
    struct Job {
        std::string correlation_id;
        scrp::ArtifactUploadRequest grant;
        Candidate candidate;
        std::chrono::steady_clock::time_point deadline;
    };
    struct Completion {
        std::string correlation_id;
        scrp::ArtifactUploadRequest grant; // token removed before queueing
        scrp::ArtifactTransferResult transfer;
    };
    bool current(const Candidate&) const;
    bool registered(const std::string&, const Candidate&) const;
    void run() noexcept;
    scrp::ArtifactTransferResult transfer(Job&);
    void cancel_locked(scrp::ArtifactError) noexcept;
    void invalidate_locked(const CandidateObservation&,
        const artifact::CandidateStatus&) noexcept;

    std::wstring output_;
    scrp::SessionContext session_;
    std::string connection_id_;
    Upload upload_;
    CurrentCandidate current_candidate_;
    const std::size_t max_candidates_, max_upload_ids_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::string, Candidate> candidates_;
    std::set<std::string> used_upload_ids_;
    std::optional<Job> pending_;
    std::deque<Completion> completed_;
    std::string active_event_id_;
    bool busy_ = false, healthy_ = true, closing_ = false;
    bool active_invalid_ = false, failed_ = false;
    scrp::ArtifactError blocked_reason_ = scrp::ArtifactError::SessionTerminated;
    std::atomic<bool> cancelled_{false}, stopped_{false};
    std::thread worker_;
};
} // namespace runner
