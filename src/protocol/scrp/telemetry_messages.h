#pragma once
#include "envelope.h"
#include <stdexcept>
#include <utility>

namespace scrp {
struct SecurityEvent {
    std::string event_id;
    std::string observed_at;
    Json::Value payload{Json::objectValue};
};

// A schema-validated storage refusal, never successful event delivery. The
// client must also correlate this event ID with the current in-flight event.
class EventStorageRejected : public std::runtime_error {
public:
    explicit EventStorageRejected(std::string event_id)
        : std::runtime_error("Host rejected security-event storage"), event_id_(std::move(event_id)) {}
    const std::string& event_id() const { return event_id_; }
private:
    std::string event_id_;
};

// W1 describes payload intent, not a frozen closed schema. The Host integration
// supplies the agreed schema. No production default invents ACK status/header fields.
// Implementations must be thread-safe and return promptly; validation can run on
// producer threads while ACK parsing runs on the client's worker.
class TelemetrySchema {
public:
    virtual ~TelemetrySchema() = default;
    virtual Json::Value channel_hello(const SessionContext&) const = 0;
    virtual void validate_event(const SecurityEvent&) const = 0;
    virtual std::string channel_ack(const Envelope&) const = 0; // validated connection_id
    // Confirms Host security-event storage under the injected contract, not
    // Artifact Broker candidate registration, upload approval or file safety.
    // The client uses the returned event_id only to remove its Pending event.
    virtual std::string event_ack(const Envelope&) const = 0;
};
void validate_event_identity(const SecurityEvent& event);
Envelope channel_hello(const SessionContext& session, const TelemetrySchema& schema);
Envelope security_event(const SessionContext& session, const std::string& connection,
                        std::uint64_t sequence, const SecurityEvent& event);
} // namespace scrp
