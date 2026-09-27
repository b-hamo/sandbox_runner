#include "session_context.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>

namespace runner {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
void fields(const Json::Value& object, std::initializer_list<const char*> names) {
    require(object.isObject() && object.size() == names.size(), "Invalid session configuration fields");
    for (const auto name : names)
        require(object.isMember(name), "Missing session configuration field");
}
std::string text(const Json::Value& value) {
    require(value.isString() && !value.asString().empty() &&
            value.asString().find('\0') == std::string::npos, "Invalid session configuration string");
    return value.asString();
}
std::wstring wide(const Json::Value& value) {
    const auto input = text(value);
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                       static_cast<int>(input.size()), nullptr, 0);
    require(size > 0, "Invalid session configuration UTF-8");
    std::wstring result(size, L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                               static_cast<int>(input.size()), &result[0], size) == size,
            "Invalid session configuration UTF-8");
    return result;
}

// Local injection adapter only. The caller supplies exact handshake expectations;
// no Host payload/status/authentication defaults are invented here. General Host
// schemas should implement TelemetrySchema and inject SessionContext directly.
// This adapter intentionally cannot publish events (outside this issue's scope).
class InjectedHandshake final : public scrp::TelemetrySchema {
    Json::Value hello_, status_, payload_;
public:
    explicit InjectedHandshake(const Json::Value& contract) {
        fields(contract, {"hello_payload", "ack_status", "ack_payload"});
        require(contract["hello_payload"].isObject() && contract["ack_payload"].isObject() &&
                (contract["ack_status"].isNull() || contract["ack_status"].isString()),
                "Invalid injected handshake contract");
        hello_ = contract["hello_payload"];
        status_ = contract["ack_status"];
        payload_ = contract["ack_payload"];
    }
    Json::Value channel_hello(const scrp::SessionContext&) const override { return hello_; }
    std::string channel_ack(const scrp::Envelope& ack) const override {
        require(ack.status == status_ && ack.error.isNull() && ack.payload == payload_ &&
                ack.connection_id.isString(), "Injected CHANNEL_ACK contract mismatch");
        return ack.connection_id.asString();
    }
    void validate_event(const scrp::SecurityEvent&) const override {
        throw std::logic_error("Local handshake injection does not provide an event contract");
    }
    std::string event_ack(const scrp::Envelope&) const override {
        throw std::logic_error("Local handshake injection does not provide an event contract");
    }
};
} // namespace

void validate_session_context(const SessionContext& context) {
    scrp::validate_session(context.session);
    telemetry::validate_connection_settings(context.connection);
    require(context.schema && context.credential, "Missing Telemetry schema or credential provider");
    // Fail before launching any Runner workers. Never expose provider exceptions,
    // which can contain credentials. The worker revalidates on every connection.
    try { telemetry::validate_credential(context.credential()); }
    catch (...) { throw std::invalid_argument("Invalid or unavailable Telemetry credential"); }
}

SessionContext load_session_context(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Cannot open session configuration");
    struct FileCloser { HANDLE handle; ~FileCloser() { CloseHandle(handle); } } closer{file};
    // Bounded read also rejects growing/oversized files without logging contents.
    std::vector<char> buffer(65537);
    DWORD count = 0;
    require(ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) &&
            count <= 65536, "Cannot read session configuration or size exceeds limit");
    const auto root = scrp::parse_json(std::string(buffer.data(), count), 65536);
    fields(root, {"session_id", "runtime_id", "generation", "endpoint", "credential", "tls", "handshake"});
    SessionContext context;
    context.session.session_id = text(root["session_id"]);
    context.session.runtime_id = text(root["runtime_id"]);
    require(root["generation"].type() == Json::uintValue || root["generation"].type() == Json::intValue,
            "Runtime generation must be a positive integer");
    require(root["generation"].isUInt64() && root["generation"].asUInt64() > 0,
            "Runtime generation must be a positive integer");
    context.session.generation = root["generation"].asUInt64();
    context.connection.endpoint = wide(root["endpoint"]);
    const auto& tls = root["tls"];
    fields(tls, {"trust", "leaf_sha256"});
    require(tls["trust"] == "windows-system" && tls["leaf_sha256"].isString(),
            "Explicit Windows TLS trust is required");
    context.connection.trust.leaf_sha256 = tls["leaf_sha256"].asString();
    const auto& credential = root["credential"];
    fields(credential, {"header_name", "header_value", "expires_unix_seconds"});
    const auto& expiry = credential["expires_unix_seconds"];
    require((expiry.type() == Json::intValue || expiry.type() == Json::uintValue) &&
            expiry.isInt64() && expiry.asInt64() > 0 &&
            expiry.asInt64() < std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::duration::max()).count(), "Invalid credential expiry");
    telemetry::ChannelCredential supplied{wide(credential["header_name"]), wide(credential["header_value"]),
        std::chrono::system_clock::time_point(std::chrono::seconds(expiry.asInt64()))};
    context.credential = [supplied] { return supplied; };
    context.schema = std::make_shared<InjectedHandshake>(root["handshake"]);
    validate_session_context(context);
    return context;
}
} // namespace runner
