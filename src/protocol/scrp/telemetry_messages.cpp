#include "telemetry_messages.h"
#include <stdexcept>

namespace scrp {
void validate_event_identity(const SecurityEvent& e) {
    if(e.event_id.empty() || e.observed_at.empty() || !e.payload.isObject() ||
       !e.payload["event_id"].isString() || e.payload["event_id"].asString()!=e.event_id ||
       !e.payload["observed_at"].isString() || e.payload["observed_at"].asString()!=e.observed_at)
        throw std::invalid_argument("Event identity/observation must match immutable payload");
}
Envelope channel_hello(const SessionContext& session,const TelemetrySchema& schema) {
    return make_envelope(session,Json::Value(),1,"CHANNEL_HELLO",schema.channel_hello(session));
}
Envelope security_event(const SessionContext& session,const std::string& connection,std::uint64_t sequence,const SecurityEvent& event) {
    validate_event_identity(event);
    auto envelope=make_envelope(session,connection,sequence,"SECURITY_EVENT",event.payload);
    if(event.payload.isMember("action_id")) envelope.action_id=event.payload["action_id"];
    return envelope;
}
} // namespace scrp
