#pragma once
#include "telemetry_messages.h"

namespace scrp {
// Explicitly selected Runner contract proposal; Host agreement is tracked in
// docs/artifact-candidate-contract.md. This does not change Envelope version.
constexpr const char* artifact_candidate_contract = "artifact-candidate-v1";
Json::Value artifact_candidate_payload(const std::string& event_id,
    const std::string& observed_at, const std::string& relative_path);
void validate_artifact_candidate(const SecurityEvent& event);
// Returns a stored event ID, throws EventStorageRejected for a valid refusal,
// or invalid_argument for malformed/unsupported ACKs. Envelope binding belongs
// to TelemetryClient, including correlation for both success and refusal.
std::string artifact_event_ack(const Envelope& ack);
} // namespace scrp
