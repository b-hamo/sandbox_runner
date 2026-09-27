#include "artifact_candidate.h"

namespace scrp {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void fields(const Json::Value& object, std::initializer_list<const char*> names) {
    require(object.isObject() && object.size() == names.size(), "Invalid artifact contract fields");
    for (const auto name : names) require(object.isMember(name), "Missing artifact contract field");
}
void validate_path(const std::string& path) {
    require(!path.empty() && valid_utf8(path), "Invalid artifact relative path encoding");
    // Output-relative Windows components. No normalization, URI decoding,
    // case folding or Unicode normalization; preserve the observed spelling.
    for (std::size_t start = 0; start < path.size();) {
        const auto end = path.find('\\', start);
        const auto part = path.substr(start, end == std::string::npos ? end : end - start);
        require(!part.empty() && part != "." && part != ".." &&
            part.back() != '.' && part.back() != ' ', "Invalid artifact path component");
        for (const unsigned char c : part)
            require(c >= 32 && c != 127 && std::string("/:*?\"<>|").find(c) == std::string::npos,
                "Invalid artifact path character");
        if (end == std::string::npos) return;
        start = end + 1;
        require(start < path.size(), "Trailing artifact path separator");
    }
}
}
void validate_artifact_candidate(const SecurityEvent& event) {
    validate_event_identity(event);
    fields(event.payload, {"event_id", "observed_at", "category", "relative_path"});
    require(valid_uuid_v4(event.event_id), "Invalid artifact event ID");
    validate_utc_timestamp(event.observed_at);
    require(event.payload["category"] == "ARTIFACT_CANDIDATE" &&
        event.payload["relative_path"].isString(), "Invalid artifact candidate payload");
    validate_path(event.payload["relative_path"].asString());
}
Json::Value artifact_candidate_payload(const std::string& id, const std::string& time, const std::string& path) {
    SecurityEvent event{id, time, Json::Value(Json::objectValue)};
    event.payload["event_id"] = id;
    event.payload["observed_at"] = time;
    event.payload["category"] = "ARTIFACT_CANDIDATE";
    event.payload["relative_path"] = path;
    validate_artifact_candidate(event);
    return event.payload;
}
std::string artifact_event_ack(const Envelope& ack) {
    require(ack.type == "EVENT_ACK", "Invalid artifact ACK type");
    fields(ack.payload, {"event_id"});
    require(ack.payload["event_id"].isString() && valid_uuid_v4(ack.payload["event_id"].asString()),
        "Invalid artifact ACK event ID");
    const auto id = ack.payload["event_id"].asString();
    if (ack.status == "STORED") {
        require(ack.error.isNull(), "Stored ACK contains an error");
        return id;
    }
    if (ack.status == "REJECTED") {
        fields(ack.error, {"code"});
        require(ack.error["code"] == "STORAGE_REJECTED", "Invalid storage refusal code");
        throw EventStorageRejected(id);
    }
    throw std::invalid_argument("Unsupported artifact ACK status");
}
} // namespace scrp
