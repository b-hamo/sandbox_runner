#pragma once
#include "artifact/candidate_detector.h"
#include "transport/telemetry/telemetry_client.h"
#include <map>

namespace runner {
// Internal conversion input, not a wire schema. IDs identify the observation
// event; a Host contract decides whether/how to expose a separate candidate ID.
struct CandidateObservation {
    std::string relative_path;
    unsigned long long file_generation = 0;
    std::string event_id;
    std::string observed_at;
};

// Implement alongside TelemetrySchema in the injected Host adapter. No default
// path/ID/invalidation payload is invented by the Runner. Calls must be prompt;
// the same observation must map to the same payload on an enqueue retry. Payloads
// must retain the supplied event_id/observed_at. Candidate payloads must carry
// ARTIFACT_CANDIDATE and the relative path/identifier per the agreed Host schema.
class CandidateEventContract {
public:
    virtual ~CandidateEventContract() = default;
    virtual Json::Value candidate_payload(const CandidateObservation&) const = 0;
    // null explicitly means no agreed invalidation wire representation. The
    // bridge still invalidates local state and reports this limitation.
    virtual Json::Value invalidation_payload(const CandidateObservation& previous,
        const artifact::CandidateStatus& cause, const std::string& event_id,
        const std::string& observed_at) const = 0;
};

// Single producer (CandidateDetector worker). Destroy only after detector join.
// No worker, socket, retry queue, file access or safety decision is owned here.
class CandidateTelemetry {
public:
    using Diagnostic = std::function<void(const char*)>;
    // In-process observer, never a Host safety verdict. true provisionally
    // registers a stable observation BEFORE Telemetry enqueue can publish it;
    // failed enqueue is rolled back with false. Mutation also emits false.
    // The callback runs on the detector worker and must return promptly.
    using Observer = std::function<void(const CandidateObservation&,
        const artifact::CandidateStatus&, bool current)>;
    CandidateTelemetry(telemetry::TelemetryClient& client,
        std::shared_ptr<const CandidateEventContract> contract, Diagnostic diagnostic,
        std::size_t max_files = 4096, Observer observer = {});
    CandidateTelemetry(const CandidateTelemetry&) = delete;
    CandidateTelemetry& operator=(const CandidateTelemetry&) = delete;
    void submit(const artifact::CandidateStatus& status) noexcept;
private:
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const;
    };
    struct Entry {
        unsigned long long generation = 0;
        CandidateObservation observation;
        bool active = false;
        bool accepted = false; // Local enqueue accepted/duplicate only; not EVENT_ACK or Broker registration.
        bool invalidated = false;
    };
    telemetry::TelemetryClient& client_;
    std::shared_ptr<const CandidateEventContract> contract_;
    Diagnostic diagnostic_;
    Observer observer_;
    std::size_t max_files_;
    std::map<std::wstring, Entry, PathLess> files_;
    bool terminal_ = false;
    void report(const char*) noexcept;
    void notify(const CandidateObservation&, const artifact::CandidateStatus&, bool);
    bool enqueue(scrp::SecurityEvent event, bool candidate);
    void invalidate(Entry&, const artifact::CandidateStatus&);
    void process(const artifact::CandidateStatus&);
};
} // namespace runner
