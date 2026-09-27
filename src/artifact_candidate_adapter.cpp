#include "artifact_candidate_adapter.h"
#include "protocol/scrp/artifact_candidate.h"

namespace runner {
ArtifactCandidateAdapter::ArtifactCandidateAdapter(std::shared_ptr<const scrp::TelemetrySchema> handshake)
    : handshake_(std::move(handshake)) {
    if (!handshake_) throw std::invalid_argument("Missing artifact adapter handshake contract");
}
Json::Value ArtifactCandidateAdapter::channel_hello(const scrp::SessionContext& session) const {
    return handshake_->channel_hello(session);
}
std::string ArtifactCandidateAdapter::channel_ack(const scrp::Envelope& ack) const {
    return handshake_->channel_ack(ack);
}
void ArtifactCandidateAdapter::validate_event(const scrp::SecurityEvent& event) const {
    scrp::validate_artifact_candidate(event);
}
std::string ArtifactCandidateAdapter::event_ack(const scrp::Envelope& ack) const {
    return scrp::artifact_event_ack(ack);
}
Json::Value ArtifactCandidateAdapter::candidate_payload(const CandidateObservation& observation) const {
    return scrp::artifact_candidate_payload(observation.event_id, observation.observed_at, observation.relative_path);
}
Json::Value ArtifactCandidateAdapter::invalidation_payload(const CandidateObservation&,
    const artifact::CandidateStatus&, const std::string&, const std::string&) const {
    return Json::Value(); // Local invalidation only; no wire message in this contract.
}
} // namespace runner
