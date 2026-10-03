#include "artifact_export_session.h"
#include <algorithm>
#include <stdexcept>

namespace runner {
namespace {
bool same_path(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool same_reported_path(const std::wstring& path, const std::string& reported) {
    const auto length = static_cast<int>(path.size());
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        path.data(), length, nullptr, 0, nullptr, nullptr);
    if (!bytes || static_cast<std::size_t>(bytes) != reported.size()) return false;
    std::string value(bytes, '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), length,
        &value[0], bytes, nullptr, nullptr) == bytes && value == reported;
}
scrp::ArtifactTransferResult failure(scrp::ArtifactError error, std::uint64_t sent = 0) {
    return {sent, error};
}
bool blocked_path_error(DWORD error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_BAD_PATHNAME ||
        error == ERROR_DIRECTORY || error == ERROR_NOT_SUPPORTED;
}
}
ArtifactExportSession::ArtifactExportSession(std::wstring output,
    scrp::SessionContext session, std::string connection_id, Upload upload,
    CurrentCandidate current_candidate, std::size_t max_candidates, std::size_t max_upload_ids)
    : output_(std::move(output)), session_(std::move(session)),
      connection_id_(std::move(connection_id)), upload_(std::move(upload)),
      current_candidate_(std::move(current_candidate)),
      max_candidates_(max_candidates), max_upload_ids_(max_upload_ids) {
    scrp::validate_session(session_);
    if (output_.empty() || connection_id_.empty() || !upload_ || !current_candidate_ ||
        !max_candidates_ || !max_upload_ids_)
        throw std::invalid_argument("Invalid artifact export session configuration");
    worker_ = std::thread(&ArtifactExportSession::run, this);
}
ArtifactExportSession::~ArtifactExportSession() { stop(); }
void ArtifactExportSession::cancel_locked(scrp::ArtifactError reason) noexcept {
    healthy_ = false;
    closing_ = true;
    blocked_reason_ = reason;
    cancelled_ = true;
    candidates_.clear();
    // A completed but unsent HTTP success is also cancelled at a session or
    // channel boundary. It cannot be replayed into a replacement connection.
    for (auto& result : completed_) result.transfer.error = reason;
    wake_.notify_all();
}
void ArtifactExportSession::set_healthy(bool healthy) noexcept {
    if (healthy) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closing_) cancel_locked(scrp::ArtifactError::UploadFailed);
}
void ArtifactExportSession::block() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel_locked(scrp::ArtifactError::SessionTerminated);
}
void ArtifactExportSession::stop() {
    block();
    if (worker_.joinable()) worker_.join();
}
void ArtifactExportSession::invalidate_locked(const CandidateObservation& observation,
    const artifact::CandidateStatus& status) noexcept {
    for (auto it = candidates_.begin(); it != candidates_.end();) {
        const bool invalid = it->first == observation.event_id ||
            (!status.path.empty() && same_path(it->second.path, status.path));
        if (!invalid) { ++it; continue; }
        if (active_event_id_ == it->first) {
            active_invalid_ = true;
            cancelled_ = true;
        }
        it = candidates_.erase(it);
    }
}
void ArtifactExportSession::observe(const CandidateObservation& observation,
    const artifact::CandidateStatus& status, bool available) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_) return;
        if (status.path.empty() && status.state == artifact::CandidateState::unavailable) {
            cancel_locked(scrp::ArtifactError::CandidateUnavailable);
            return;
        }
        if (!available || status.state != artifact::CandidateState::candidate ||
            status.error != ERROR_SUCCESS || !status.has_observation ||
            !artifact::valid_relative_file(status.path) || !status.generation ||
            status.generation != observation.file_generation ||
            !same_reported_path(status.path, observation.relative_path) ||
            !scrp::valid_uuid_v4(observation.event_id)) {
            invalidate_locked(observation, status);
            return;
        }
        const auto found = candidates_.find(observation.event_id);
        if (found != candidates_.end()) {
            if (!same_path(found->second.path, status.path) ||
                found->second.generation != status.generation ||
                !artifact::same_observation(found->second.observation, status.observation))
                cancel_locked(scrp::ArtifactError::Internal);
            return;
        }
        if (candidates_.size() >= max_candidates_) {
            cancel_locked(scrp::ArtifactError::Internal);
            return;
        }
        // A new candidate for the same spelling invalidates its former event.
        invalidate_locked({}, status);
        candidates_.emplace(observation.event_id,
            Candidate{status.path, status.generation, status.observation});
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        cancel_locked(scrp::ArtifactError::Internal);
    }
}
bool ArtifactExportSession::current(const Candidate& expected) const {
    artifact::CandidateStatus status;
    return current_candidate_(expected.path, status) &&
        same_path(status.path, expected.path) && status.generation == expected.generation &&
        status.state == artifact::CandidateState::candidate && status.error == ERROR_SUCCESS &&
        status.has_observation && artifact::same_observation(status.observation, expected.observation);
}
bool ArtifactExportSession::registered(const std::string& event, const Candidate& expected) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = candidates_.find(event);
    return healthy_ && !closing_ && !active_invalid_ && found != candidates_.end() &&
        found->second.generation == expected.generation && same_path(found->second.path, expected.path) &&
        artifact::same_observation(found->second.observation, expected.observation);
}
::control::Replies ArtifactExportSession::handle(const scrp::Envelope& envelope) {
    // Validation is repeated at this seam; invalid requests never enter worker
    // state. The caller's Receiver also rejects schema/session violations.
    auto grant = scrp::parse_artifact_request(envelope);
    if (envelope.session.session_id != session_.session_id ||
        envelope.session.runtime_id != session_.runtime_id ||
        envelope.session.generation != session_.generation ||
        envelope.connection_id != connection_id_ || !scrp::valid_uuid_v4(envelope.message_id))
        throw std::invalid_argument("Artifact request session binding rejected");
    auto immediate = [&](scrp::ArtifactError error) {
        return ::control::Replies{{scrp::artifact_result_reply(grant, failure(error))}};
    };
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || !healthy_) return immediate(blocked_reason_);
    if (used_upload_ids_.find(grant.upload_id) != used_upload_ids_.end() ||
        used_upload_ids_.size() >= max_upload_ids_)
        return immediate(scrp::ArtifactError::Blocked);
    used_upload_ids_.insert(grant.upload_id); // all valid grants are one attempt
    if (busy_ || !completed_.empty()) return immediate(scrp::ArtifactError::Busy);
    const auto found = candidates_.find(grant.candidate_event_id);
    if (found == candidates_.end()) return immediate(scrp::ArtifactError::CandidateUnavailable);
    if (grant.deadline <= std::chrono::system_clock::now() ||
        grant.monotonic_deadline <= std::chrono::steady_clock::now())
        return immediate(scrp::ArtifactError::UploadExpired);
    // A monotonic budget starts at admission, before worker scheduling and file
    // access. A backward wall-clock adjustment cannot extend this grant.
    const auto cutoff = grant.monotonic_deadline;
    Job job{envelope.message_id, std::move(grant), found->second,
        cutoff};
    pending_ = std::move(job);
    active_event_id_ = pending_->grant.candidate_event_id;
    active_invalid_ = false;
    cancelled_ = false;
    busy_ = true;
    wake_.notify_one();
    return {{}, false, true}; // final ARTIFACT_RESULT only; no initial ACK
}
scrp::ArtifactTransferResult ArtifactExportSession::transfer(Job& job) {
    auto expired = [&] { return std::chrono::steady_clock::now() >= job.deadline ||
        std::chrono::system_clock::now() >= job.grant.deadline; };
    if (expired()) return failure(scrp::ArtifactError::UploadExpired);
    if (cancelled_ || !registered(job.grant.candidate_event_id, job.candidate) || !current(job.candidate))
        return failure(scrp::ArtifactError::CandidateUnavailable);
    artifact::VerifiedFile file;
    const auto error = artifact::open_verified_file(output_, job.candidate.path,
        job.candidate.observation, file);
    if (error) return failure(blocked_path_error(error) ? scrp::ArtifactError::Blocked :
        scrp::ArtifactError::CandidateUnavailable);
    if (file.size() > job.grant.max_bytes || file.size() > scrp::artifact_max_bytes)
        return failure(scrp::ArtifactError::Blocked);
    if (cancelled_ || !registered(job.grant.candidate_event_id, job.candidate) || !current(job.candidate))
        return failure(scrp::ArtifactError::CandidateUnavailable);
    if (expired()) return failure(scrp::ArtifactError::UploadExpired);
    auto result = upload_(job.grant, file, cancelled_);
    if (result.bytes_sent > file.size()) return failure(scrp::ArtifactError::Internal, file.size());
    if (result.error == scrp::ArtifactError::None &&
        (result.bytes_sent != file.size() || file.validate_unchanged() != ERROR_SUCCESS))
        return failure(scrp::ArtifactError::CandidateUnavailable, result.bytes_sent);
    if (expired()) return failure(scrp::ArtifactError::UploadExpired, result.bytes_sent);
    return result;
}
void ArtifactExportSession::run() noexcept {
    try {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return closing_ || pending_.has_value(); });
                if (!pending_) break;
                job = std::move(*pending_);
                pending_.reset();
            }
            scrp::ArtifactTransferResult result;
            try { result = transfer(job); }
            catch (...) { result = failure(scrp::ArtifactError::Internal); }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (closing_) result.error = blocked_reason_;
                else if (active_invalid_) result.error = scrp::ArtifactError::CandidateUnavailable;
                if (!completed_.empty()) throw std::runtime_error("Artifact completion slot full");
                job.grant.upload_token.clear();
                completed_.push_back({std::move(job.correlation_id), std::move(job.grant), result});
                active_event_id_.clear();
                active_invalid_ = false;
                busy_ = false;
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_ = true;
        cancel_locked(scrp::ArtifactError::Internal);
    }
    stopped_ = true;
}
std::vector<::control::DeferredReply> ArtifactExportSession::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) throw std::runtime_error("Artifact completion delivery failed");
    std::vector<::control::DeferredReply> result;
    while (!completed_.empty()) {
        auto& completion = completed_.front();
        result.push_back({completion.correlation_id,
            scrp::artifact_result_reply(completion.grant, completion.transfer)});
        completed_.pop_front();
    }
    return result;
}
} // namespace runner
