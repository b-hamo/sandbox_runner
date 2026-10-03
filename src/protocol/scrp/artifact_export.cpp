#include "artifact_export.h"
#include <windows.h>
#include <algorithm>

namespace scrp {
namespace {
void check(bool ok) { if (!ok) throw std::invalid_argument("Artifact export schema mismatch"); }
void fields(const Json::Value& object, std::initializer_list<const char*> names) {
    check(object.isObject() && object.size() == names.size());
    for (const auto name : names) check(object.isMember(name));
}
bool number(const Json::Value& v, std::uint64_t low, std::uint64_t high) {
    return (v.type() == Json::intValue || v.type() == Json::uintValue) &&
        v.isUInt64() && v.asUInt64() >= low && v.asUInt64() <= high;
}
bool upload_id(const std::string& value) {
    return value.size() >= 16 && value.size() <= 128 && value.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}
bool token(const std::string& value) {
    // 32 random bytes encoded as canonical, unpadded base64url.
    return value.size() == 43 && value.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos &&
        std::string("AEIMQUYcgkosw048").find(value.back()) != std::string::npos;
}
bool host_id(const std::string& value) {
    const std::string alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    return !value.empty() && value.size() <= 64 && alpha.find(value.front()) != std::string::npos &&
        value.find_first_not_of(alpha+"._:-") == std::string::npos;
}
void session_identity(const SessionContext& s) {
    validate_session(s); check(host_id(s.session_id) && host_id(s.runtime_id));
}
void artifact_identity(const Envelope& e) {
    session_identity(e.session);
    check(e.task_id.isNull() && e.action_id.isNull() && e.connection_id.isString() &&
        host_id(e.connection_id.asString()));
}
bool same_session(const SessionContext& a, const SessionContext& b) {
    return a.session_id == b.session_id && a.runtime_id == b.runtime_id && a.generation == b.generation;
}
const char* error_code(ArtifactError error) {
    switch (error) {
    case ArtifactError::CandidateUnavailable: return "CANDIDATE_UNAVAILABLE";
    case ArtifactError::Busy: return "ARTIFACT_BUSY";
    case ArtifactError::Blocked: return "ARTIFACT_BLOCKED";
    case ArtifactError::UploadFailed: return "UPLOAD_FAILED";
    case ArtifactError::UploadExpired: return "UPLOAD_EXPIRED";
    case ArtifactError::SessionTerminated: return "SESSION_TERMINATED";
    default: return "INTERNAL";
    }
}
const char* error_message(ArtifactError error) {
    switch (error) {
    case ArtifactError::CandidateUnavailable: return "Candidate unavailable";
    case ArtifactError::Busy: return "Artifact upload busy";
    case ArtifactError::Blocked: return "Artifact upload blocked";
    case ArtifactError::UploadFailed: return "Artifact upload failed";
    case ArtifactError::UploadExpired: return "Artifact upload expired";
    case ArtifactError::SessionTerminated: return "Session terminated";
    default: return "Artifact internal failure";
    }
}
void validate_result(const Envelope& e) {
    artifact_identity(e);
    check(e.correlation_id.isString() && valid_uuid_v4(e.correlation_id.asString()));
    const auto& p = e.payload;
    fields(p,{"candidate_event_id","upload_id","transfer","bytes_sent"});
    check(p["candidate_event_id"].isString() && valid_uuid_v4(p["candidate_event_id"].asString()) &&
        p["upload_id"].isString() && upload_id(p["upload_id"].asString()) &&
        number(p["bytes_sent"],0,artifact_max_bytes));
    if (p["transfer"] == "UPLOADED") { check(e.status == "OK" && e.error.isNull()); return; }
    check(e.status == "ERROR" && (p["transfer"] == "FAILED" || p["transfer"] == "EXPIRED"));
    fields(e.error,{"code","message","retryable","recommended_next_step"});
    check(e.error["message"].isString() && !e.error["message"].asString().empty() &&
        e.error["message"].asString().size() <= 256 && valid_utf8(e.error["message"].asString()) &&
        e.error["message"].asString().find('\0') == std::string::npos &&
        e.error["retryable"] == false && e.error["recommended_next_step"].isNull());
    bool found = false;
    for (const auto error : {ArtifactError::CandidateUnavailable,ArtifactError::Busy,ArtifactError::Blocked,
            ArtifactError::UploadFailed,ArtifactError::UploadExpired,ArtifactError::SessionTerminated,ArtifactError::Internal})
        if (e.error["code"] == error_code(error)) {
            check((p["transfer"] == "EXPIRED") == (error == ArtifactError::UploadExpired)); found = true;
        }
    check(found);
}
}

std::chrono::system_clock::time_point artifact_utc_time(const std::string& stamp) {
    validate_utc_timestamp(stamp);
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(std::stoi(stamp.substr(0,4)));
    utc.wMonth = static_cast<WORD>(std::stoi(stamp.substr(5,2)));
    utc.wDay = static_cast<WORD>(std::stoi(stamp.substr(8,2)));
    utc.wHour = static_cast<WORD>(std::stoi(stamp.substr(11,2)));
    utc.wMinute = static_cast<WORD>(std::stoi(stamp.substr(14,2)));
    utc.wSecond = static_cast<WORD>(std::stoi(stamp.substr(17,2)));
    FILETIME file{}; check(SystemTimeToFileTime(&utc,&file));
    constexpr std::int64_t epoch = 116444736000000000LL;
    const auto ticks = (std::uint64_t(file.dwHighDateTime)<<32) | file.dwLowDateTime;
    std::int64_t micros = (static_cast<std::int64_t>(ticks)-epoch)/10;
    if (stamp.size() != 20) {
        auto fraction = stamp.substr(20,stamp.size()-21); fraction.append(6-fraction.size(),'0');
        micros += std::stoll(fraction);
    }
    using Clock = std::chrono::system_clock;
    const auto low = std::chrono::duration_cast<std::chrono::microseconds>(Clock::duration::min()).count();
    const auto high = std::chrono::duration_cast<std::chrono::microseconds>(Clock::duration::max()).count();
    // Valid UTC dates may exceed this implementation's clock representation.
    // Saturating preserves past/future meaning without signed duration overflow.
    if (micros <= low) return Clock::time_point::min();
    if (micros >= high) return Clock::time_point::max();
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(micros)));
}
ArtifactUploadRequest parse_artifact_request(const Envelope& e) {
    check(e.type == "ARTIFACT_REQUEST"); artifact_identity(e);
    check(e.correlation_id.isNull() && e.status.isNull() && e.error.isNull());
    const auto& p = e.payload;
    fields(p,{"candidate_event_id","upload_id","upload_token","upload_deadline_at","max_bytes"});
    check(p["candidate_event_id"].isString() && valid_uuid_v4(p["candidate_event_id"].asString()) &&
        p["upload_id"].isString() && upload_id(p["upload_id"].asString()) &&
        p["upload_token"].isString() && token(p["upload_token"].asString()) &&
        p["upload_deadline_at"].isString() && number(p["max_bytes"],1,artifact_max_bytes));
    ArtifactUploadRequest request{p["candidate_event_id"].asString(),p["upload_id"].asString(),p["upload_token"].asString(),
        artifact_utc_time(p["upload_deadline_at"].asString()),p["max_bytes"].asUInt64(),{}};
    const auto wall = std::chrono::system_clock::now();
    const auto monotonic = std::chrono::steady_clock::now();
    request.monotonic_deadline = monotonic;
    if (request.deadline > wall) {
        const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(request.deadline-wall);
        const auto available = std::chrono::steady_clock::time_point::max()-monotonic;
        request.monotonic_deadline += std::min(budget,available);
    }
    return request;
}
control::Reply artifact_result_reply(const ArtifactUploadRequest& request, ArtifactTransferResult result) {
    check(valid_uuid_v4(request.candidate_event_id) && upload_id(request.upload_id) && result.bytes_sent <= artifact_max_bytes);
    control::Reply reply; reply.type = "ARTIFACT_RESULT";
    reply.payload["candidate_event_id"] = request.candidate_event_id;
    reply.payload["upload_id"] = request.upload_id;
    reply.payload["bytes_sent"] = Json::UInt64(result.bytes_sent);
    reply.payload["transfer"] = result.error == ArtifactError::None ? "UPLOADED" :
        result.error == ArtifactError::UploadExpired ? "EXPIRED" : "FAILED";
    if (result.error != ArtifactError::None) {
        reply.status = "ERROR";
        reply.error["code"] = error_code(result.error);
        reply.error["message"] = error_message(result.error);
        reply.error["retryable"] = false;
        reply.error["recommended_next_step"] = Json::Value();
    }
    return reply;
}
ArtifactExportSchema::ArtifactExportSchema(std::shared_ptr<const control::Schema> delegate) : delegate_(std::move(delegate)) {
    check(delegate_ != nullptr);
}
Json::Value ArtifactExportSchema::hello(const SessionContext& session) const {
    auto payload = delegate_->hello(session);
    check(payload["capabilities"].isArray());
    check(std::find(payload["capabilities"].begin(),payload["capabilities"].end(),Json::Value(artifact_export_capability)) == payload["capabilities"].end());
    payload["capabilities"].append(artifact_export_capability);
    session_identity(session);
    std::lock_guard<std::mutex> lock(mutex_); session_ = session; connection_id_.clear(); negotiated_ = false; telemetry_ = {};
    return payload;
}
std::string ArtifactExportSchema::hello_ack(const Envelope& e) const {
    check(e.type == "HELLO_ACK" && e.sequence_number == 1 && e.task_id.isNull() && e.action_id.isNull() &&
        e.correlation_id.isString() && valid_uuid_v4(e.correlation_id.asString()));
    auto compatible = e;
    const auto& allowed = e.payload["allowed_capabilities"];
    check(allowed.isArray() && allowed.size() <= 32);
    compatible.payload["allowed_capabilities"] = Json::Value(Json::arrayValue);
    bool enabled = false;
    for (const auto& value : allowed) {
        if (value == artifact_export_capability) { check(!enabled); enabled = true; }
        else compatible.payload["allowed_capabilities"].append(value);
    }
    // Existing schema still validates every other HELLO_ACK field/capability.
    const auto connection = delegate_->hello_ack(compatible);
    const auto& credential = e.payload["channel_credentials"]["telemetry"];
    fields(credential,{"token","expires_at"});
    check(credential["token"].isString() && credential["expires_at"].isString());
    const auto secret = credential["token"].asString();
    check(secret.size() >= 32 && secret.size() <= 512 && secret.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos);
    telemetry::ChannelCredential channel{L"Authorization",L"Bearer " + std::wstring(secret.begin(),secret.end()),
        artifact_utc_time(credential["expires_at"].asString())};
    telemetry::validate_credential(channel);
    std::lock_guard<std::mutex> lock(mutex_); check(same_session(session_,e.session));
    negotiated_ = enabled; connection_id_ = connection; telemetry_ = enabled ? channel : telemetry::ChannelCredential{};
    return connection;
}
bool ArtifactExportSchema::artifact_allowed() const { std::lock_guard<std::mutex> lock(mutex_); return negotiated_; }
telemetry::ChannelCredential ArtifactExportSchema::telemetry_credential() const {
    std::lock_guard<std::mutex> lock(mutex_); check(negotiated_); telemetry::validate_credential(telemetry_); return telemetry_;
}
void ArtifactExportSchema::validate_request(const Envelope& e) const {
    if (e.type != "ARTIFACT_REQUEST") { delegate_->validate_request(e); return; }
    parse_artifact_request(e);
    std::lock_guard<std::mutex> lock(mutex_); check(negotiated_ && same_session(session_,e.session) && e.connection_id == connection_id_);
}
void ArtifactExportSchema::validate_reply(const Envelope& e) const {
    if (e.type != "ARTIFACT_RESULT") { delegate_->validate_reply(e); return; }
    validate_result(e);
    std::lock_guard<std::mutex> lock(mutex_); check(negotiated_ && same_session(session_,e.session) && e.connection_id == connection_id_);
}
Json::Value ExportTelemetrySchema::channel_hello(const SessionContext& s) const {
    session_identity(s); Json::Value p(Json::objectValue); p["channel"] = "telemetry"; return p;
}
void ExportTelemetrySchema::validate_event(const SecurityEvent& e) const { validate_artifact_candidate(e); }
std::string ExportTelemetrySchema::channel_ack(const Envelope& e) const {
    artifact_identity(e);
    check(e.type == "CHANNEL_ACK" && e.sequence_number == 1 && e.status == "OK" && e.error.isNull() &&
        e.connection_id.isString() && !e.connection_id.asString().empty() && e.task_id.isNull() && e.action_id.isNull());
    fields(e.payload,{"channel","max_event_bytes"});
    check(e.payload["channel"] == "telemetry" && number(e.payload["max_event_bytes"],16384,16384));
    return e.connection_id.asString();
}
std::string ExportTelemetrySchema::event_ack(const Envelope& e) const {
    artifact_identity(e); return artifact_event_ack(e);
}
} // namespace scrp
