#pragma once

#include <json/json.h>
#include <cstdint>
#include <string>

namespace scrp {
struct SessionContext {
    std::string session_id;
    std::string runtime_id;
    std::uint64_t generation = 0; // Host Runtime generation, never a file generation.
};

struct Envelope {
    std::string version = "1.0";
    SessionContext session;
    Json::Value connection_id; // null before CHANNEL_ACK
    std::string message_id;
    Json::Value task_id;
    Json::Value action_id;
    std::uint64_t sequence_number = 0;
    std::string timestamp;
    std::string nonce;
    std::string type;
    Json::Value correlation_id;
    Json::Value status;
    Json::Value error;
    Json::Value payload{Json::objectValue};
};

std::string uuid_v4();
std::string random_nonce();
std::string utc_now();
bool valid_utf8(const std::string& value);
bool valid_uuid_v4(const std::string& value);
void validate_utc_timestamp(const std::string& value);
std::string json_text(const Json::Value& value);
Json::Value parse_json(const std::string& text, std::size_t max_bytes);
std::string serialize(const Envelope& envelope);
Envelope parse_envelope(const std::string& text, std::size_t max_bytes = 65536);
void validate_session(const SessionContext& session);
void validate_incoming(const Envelope& envelope, const SessionContext& session,
                       std::uint64_t expected_sequence);
Envelope make_envelope(const SessionContext& session, const Json::Value& connection,
                       std::uint64_t sequence, const std::string& type,
                       const Json::Value& payload);
} // namespace scrp
