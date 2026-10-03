#pragma once
#include "artifact_candidate.h"
#include "transport/control/control_receiver.h"
#include <mutex>

namespace scrp {
constexpr const char* artifact_export_contract = "artifact-export-v1";
constexpr const char* artifact_export_capability = "artifact.export.v1";
constexpr std::uint64_t artifact_max_bytes = 52428800;

struct ArtifactUploadRequest {
    std::string candidate_event_id, upload_id, upload_token;
    std::chrono::system_clock::time_point deadline;
    std::uint64_t max_bytes = 0;
    // Internal admission-time bound, never serialized into the five fields.
    std::chrono::steady_clock::time_point monotonic_deadline;
};
enum class ArtifactError {
    None, CandidateUnavailable, Busy, Blocked, UploadFailed, UploadExpired,
    SessionTerminated, Internal
};
struct ArtifactTransferResult {
    std::uint64_t bytes_sent = 0; // body bytes accepted by WinHTTP, not Host receipt
    ArtifactError error = ArtifactError::Internal;
};
std::chrono::system_clock::time_point artifact_utc_time(const std::string&);
ArtifactUploadRequest parse_artifact_request(const Envelope&);
control::Reply artifact_result_reply(const ArtifactUploadRequest&, ArtifactTransferResult);

// Adds only the explicitly selected artifact profile to the existing Control
// schema. GUI and management validation continue through the original schema.
// HELLO_ACK's credentials become available only after full validation.
class ArtifactExportSchema final : public control::Schema {
public:
    explicit ArtifactExportSchema(std::shared_ptr<const control::Schema> delegate);
    Json::Value hello(const SessionContext&) const override;
    std::string hello_ack(const Envelope&) const override;
    void validate_request(const Envelope&) const override;
    void validate_reply(const Envelope&) const override;
    std::size_t message_limit() const override { return delegate_->message_limit(); }
    bool artifact_allowed() const;
    telemetry::ChannelCredential telemetry_credential() const;
private:
    std::shared_ptr<const control::Schema> delegate_;
    mutable std::mutex mutex_;
    mutable SessionContext session_;
    mutable std::string connection_id_;
    mutable bool negotiated_ = false;
    mutable telemetry::ChannelCredential telemetry_;
};

class ExportTelemetrySchema final : public TelemetrySchema {
public:
    Json::Value channel_hello(const SessionContext&) const override;
    void validate_event(const SecurityEvent&) const override;
    std::string channel_ack(const Envelope&) const override;
    std::string event_ack(const Envelope&) const override;
};
} // namespace scrp
