#pragma once
#include "candidate_telemetry.h"

namespace runner {
// Adds the explicitly selected artifact event contract to an existing channel
// handshake schema. Immutable after construction; shared with both workers.
class ArtifactCandidateAdapter final : public scrp::TelemetrySchema, public CandidateEventContract {
public:
    explicit ArtifactCandidateAdapter(std::shared_ptr<const scrp::TelemetrySchema> handshake);
    Json::Value channel_hello(const scrp::SessionContext&) const override;
    std::string channel_ack(const scrp::Envelope&) const override;
    void validate_event(const scrp::SecurityEvent&) const override;
    std::string event_ack(const scrp::Envelope&) const override;
    Json::Value candidate_payload(const CandidateObservation&) const override;
    Json::Value invalidation_payload(const CandidateObservation&, const artifact::CandidateStatus&,
        const std::string&, const std::string&) const override;
private:
    std::shared_ptr<const scrp::TelemetrySchema> handshake_;
};
} // namespace runner
