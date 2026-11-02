#include "artifact_export_session.h"
#include <algorithm>
#include <stdexcept>
#include "control/control_util.h"

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
    std::lock_guard<std::mutex> lock(mutex_);
    release_snapshot_locked();
}
void ArtifactExportSession::invalidate_locked(const CandidateObservation& observation,
    const artifact::CandidateStatus& status) noexcept {
    for (auto it = candidates_.begin(); it != candidates_.end();) {
        // Captured files have their own identity and held handles. Watcher
        // notifications cannot invalidate or replace an authenticated manifest.
        if (it->second.snapshot) { ++it; continue; }
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
    if (expected.snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!snapshot_ || closing_) return false;
        for (const auto& file:snapshot_->files())
            if (file.relative_path==expected.path &&
                artifact::same_observation(file.source.observation(),expected.observation))
                return file.source.validate_unchanged()==ERROR_SUCCESS;
        return false;
    }
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
    if (busy_ || !completed_.empty() || completed_snapshot_) return immediate(scrp::ArtifactError::Busy);
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
namespace {
::control::Reply snapshot_reply(const scrp::Envelope& request, const char* problem=nullptr) {
    ::control::Reply reply; reply.type="OUTPUT_SNAPSHOT_RESULT";
    reply.payload["snapshot_id"]=request.payload["snapshot_id"];
    reply.payload["operation"]=request.payload["operation"];
    reply.payload["complete"]=problem==nullptr;
    reply.payload["problem"]=problem ? Json::Value(problem) : Json::Value();
    reply.payload["files"]=Json::Value(Json::arrayValue);
    return reply;
}
}
::control::Replies ArtifactExportSession::snapshot(const scrp::Envelope& request) {
    scrp::validate_output_snapshot_request(request);
    if (request.session.session_id!=session_.session_id || request.session.runtime_id!=session_.runtime_id ||
        request.session.generation!=session_.generation || request.connection_id!=connection_id_ ||
        !scrp::valid_uuid_v4(request.message_id))
        throw std::invalid_argument("Output snapshot session binding rejected");
    std::lock_guard<std::mutex> lock(mutex_);
    if (closing_ || !healthy_) return {{snapshot_reply(request,"CHANNEL_UNAVAILABLE")}};
    if (busy_ || !completed_.empty() || completed_snapshot_)
        return {{snapshot_reply(request,"ARTIFACT_BUSY")}};
    pending_snapshot_=request;
    busy_=true;
    wake_.notify_one();
    return {{},false,true};
}
void ArtifactExportSession::release_snapshot_locked() {
    for(auto it=candidates_.begin();it!=candidates_.end();)
        if(it->second.snapshot)it=candidates_.erase(it);else ++it;
    snapshot_.reset();snapshot_id_.clear();
}
::control::Reply ArtifactExportSession::snapshot_work(const scrp::Envelope& request) {
    const auto operation=request.payload["operation"].asString();
    const auto id=request.payload["snapshot_id"].asString();
    if(operation!="CAPTURE") {
        // Only this worker mutates the snapshot. Reader and detector access is
        // synchronized; no upload can overlap VERIFY or RELEASE.
        std::lock_guard<std::mutex> lock(mutex_);
        // RELEASE is idempotent when no snapshot is held: the Host may have
        // lost either the CAPTURE reply or a previous RELEASE reply. Never
        // release a different live snapshot on that account.
        if(operation=="RELEASE" && !snapshot_)return snapshot_reply(request);
        if(!snapshot_ || snapshot_id_!=id)return snapshot_reply(request,"SNAPSHOT_UNAVAILABLE");
        if(operation=="RELEASE") {release_snapshot_locked();return snapshot_reply(request);}
        return snapshot_reply(request,snapshot_->validate() ? "OUTPUT_CHANGED" : nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(snapshot_)return snapshot_reply(request,"SNAPSHOT_ACTIVE");
    }
    auto captured=std::make_unique<artifact::OutputSnapshot>(output_);
    const auto error=captured->capture(request.payload["max_file_bytes"].asUInt64(),
        request.payload["max_total_bytes"].asUInt64());
    if(error)return snapshot_reply(request,error==ERROR_FILE_TOO_LARGE ? "OUTPUT_TOO_LARGE" : "OUTPUT_UNAVAILABLE");
    auto reply=snapshot_reply(request);
    for(const auto& file:captured->files()) {
        Json::Value item(Json::objectValue);
        item["candidate_event_id"]=file.event_id;
        item["relative_path"]=control::util::wide_to_utf8(file.relative_path);
        item["size_bytes"]=Json::UInt64(file.source.size());
        item["sha256"]=file.sha256;
        reply.payload["files"].append(item);
        if(scrp::json_text(reply.payload).size()>scrp::output_manifest_max_bytes)
            return snapshot_reply(request,"MANIFEST_TOO_LARGE");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if(closing_ || !healthy_)return snapshot_reply(request,"CHANNEL_UNAVAILABLE");
    if(candidates_.size()+captured->files().size()>max_candidates_)
        return snapshot_reply(request,"CANDIDATE_STORAGE_FULL");
    for(const auto& file:captured->files())
        candidates_.emplace(file.event_id,Candidate{file.relative_path,1,file.source.observation(),true});
    snapshot_id_=id;snapshot_=std::move(captured);
    return reply;
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
            std::optional<scrp::Envelope> snapshot_request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return closing_ || pending_.has_value() || pending_snapshot_.has_value(); });
                if (pending_snapshot_) {snapshot_request=std::move(pending_snapshot_);pending_snapshot_.reset();}
                else if (pending_) {job = std::move(*pending_);pending_.reset();}
                else break;
            }
            if(snapshot_request) {
                ::control::Reply reply;
                try {reply=snapshot_work(*snapshot_request);}
                catch(...) {reply=snapshot_reply(*snapshot_request,"SNAPSHOT_FAILED");}
                std::lock_guard<std::mutex> lock(mutex_);
                if(closing_)reply=snapshot_reply(*snapshot_request,"CHANNEL_UNAVAILABLE");
                completed_snapshot_=::control::DeferredReply{snapshot_request->message_id,std::move(reply)};
                busy_=false;
                continue;
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
    if(completed_snapshot_) {result.push_back(std::move(*completed_snapshot_));completed_snapshot_.reset();}
    while (!completed_.empty()) {
        auto& completion = completed_.front();
        result.push_back({completion.correlation_id,
            scrp::artifact_result_reply(completion.grant, completion.transfer)});
        completed_.pop_front();
    }
    return result;
}
} // namespace runner
