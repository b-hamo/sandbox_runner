#include "candidate_telemetry.h"
#include "artifact/file_stability.h"
#include <stdexcept>

namespace runner {
namespace {
std::string utf8_path(const std::wstring& path) {
    if (path.size() > 32760 || !artifact::valid_relative_file(path))
        throw std::invalid_argument("Invalid candidate path");
    const auto length = static_cast<int>(path.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        path.data(), length, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::invalid_argument("Invalid candidate Unicode");
    std::string result(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), length,
            &result[0], size, nullptr, nullptr) != size)
        throw std::invalid_argument("Invalid candidate Unicode");
    return result; // JSON serialization, not console escaping, handles the path.
}
}
bool CandidateTelemetry::PathLess::operator()(const std::wstring& a, const std::wstring& b) const {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}
CandidateTelemetry::CandidateTelemetry(telemetry::TelemetryClient& client,
    std::shared_ptr<const CandidateEventContract> contract, Diagnostic diagnostic, std::size_t max_files,
    Observer observer)
    : client_(client), contract_(std::move(contract)), diagnostic_(std::move(diagnostic)),
      observer_(std::move(observer)), max_files_(max_files) {
    if (!max_files_ || !diagnostic_) throw std::invalid_argument("Missing candidate bridge limits/diagnostics");
    if (!contract_) report("Artifact telemetry unavailable: no CandidateEventContract supplied");
}
void CandidateTelemetry::report(const char* message) noexcept {
    try { diagnostic_(message); } catch (...) {}
}
void CandidateTelemetry::notify(const CandidateObservation& observation,
    const artifact::CandidateStatus& status, bool current) {
    if (observer_) observer_(observation, status, current);
}
bool CandidateTelemetry::enqueue(scrp::SecurityEvent event, bool candidate) {
    scrp::validate_event_identity(event);
    if (candidate && event.payload["category"] != "ARTIFACT_CANDIDATE")
        throw std::invalid_argument("Invalid candidate category");
    switch (client_.enqueue(event)) {
    case telemetry::EnqueueResult::accepted:
    case telemetry::EnqueueResult::duplicate: return true;
    case telemetry::EnqueueResult::buffer_full: report("Artifact telemetry rejected: Pending buffer full"); break;
    case telemetry::EnqueueResult::event_too_large: report("Artifact telemetry rejected: event too large"); break;
    case telemetry::EnqueueResult::stopped: report("Artifact telemetry rejected: client stopped"); break;
    case telemetry::EnqueueResult::conflict: report("Artifact telemetry rejected: event ID conflict"); break;
    case telemetry::EnqueueResult::invalid: report("Artifact telemetry rejected: schema validation failed"); break;
    }
    return false;
}
void CandidateTelemetry::invalidate(Entry& entry, const artifact::CandidateStatus& cause) {
    if (!entry.active) return;
    entry.active = false; // Local invalidation survives mapping/enqueue failures.
    notify(entry.observation, cause, false);
    if (!entry.accepted || !contract_) return;
    try {
        const auto id = scrp::uuid_v4(), time = scrp::utc_now();
        auto payload = contract_->invalidation_payload(entry.observation, cause, id, time);
        if (payload.isNull()) {
            report("Artifact invalidated locally; no agreed invalidation wire contract");
            return;
        }
        enqueue({id, time, std::move(payload)}, false);
    } catch (...) {
        report("Artifact invalidation reporting failed; local candidate is invalid");
    }
}
void CandidateTelemetry::process(const artifact::CandidateStatus& status) {
    if (terminal_) return;
    if (status.path.empty() && status.state == artifact::CandidateState::unavailable) {
        terminal_ = true;
        notify({}, status, false);
        for (auto& item : files_) invalidate(item.second, status);
        return;
    }
    const auto path = utf8_path(status.path);
    if (!status.generation) throw std::invalid_argument("Missing file generation");
    auto found = files_.find(status.path);
    if (found == files_.end()) {
        if (files_.size() >= max_files_) {
            report("Artifact telemetry state limit reached; candidate not reported");
            return;
        }
        found = files_.emplace(status.path, Entry{}).first;
    }
    auto& entry = found->second;
    if (status.generation < entry.generation) return;
    if (status.generation > entry.generation) {
        invalidate(entry, status);
        entry = Entry{};
        entry.generation = status.generation;
    }
    if (status.state != artifact::CandidateState::candidate || status.error != ERROR_SUCCESS) {
        if (entry.active || status.state == artifact::CandidateState::unavailable)
            entry.invalidated = true;
        invalidate(entry, status);
        return;
    }
    if (entry.accepted || entry.invalidated) return; // Survives ACK removal from the Pending store.
    entry.active = true;
    if (!contract_) {
        report("Artifact candidate not reported: no CandidateEventContract supplied");
        return;
    }
    if (entry.observation.event_id.empty())
        entry.observation = {path, status.generation, scrp::uuid_v4(), scrp::utc_now()};
    const auto& observation = entry.observation;
    scrp::SecurityEvent event{observation.event_id, observation.observed_at,
        contract_->candidate_payload(observation)};
    // The Telemetry worker can send immediately after enqueue returns (or even
    // before it returns). Install the file observation first so a Host request
    // responding to that event cannot overtake local candidate registration.
    notify(observation, status, true);
    entry.accepted = enqueue(std::move(event), true);
    if (!entry.accepted) notify(observation, status, false);
}
void CandidateTelemetry::submit(const artifact::CandidateStatus& status) noexcept {
    try { process(status); }
    catch (...) {
        // Export observers fail closed even when conversion or their callback
        // fails. The optional observer leaves legacy candidate mode unchanged.
        try { notify({}, status, false); } catch (...) {}
        report("Artifact telemetry conversion failed; candidate not reported");
    }
}
} // namespace runner
