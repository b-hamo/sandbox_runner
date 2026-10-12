#pragma once
#include "protocol/scrp/telemetry_messages.h"
#include <chrono>
#include <deque>
#include <vector>

namespace telemetry {
using Clock = std::chrono::steady_clock;
struct Limits {
    std::size_t event_bytes = 16 * 1024;
    std::size_t buffer_bytes = 10 * 1024 * 1024;
    std::chrono::milliseconds retention{300000};
};
enum class EnqueueResult { accepted, duplicate, conflict, event_too_large, buffer_full, stopped, invalid };

// Not internally synchronized: TelemetryClient owns the lock. byte_charge includes
// the full serialized Envelope allowance, not just payload. Expiry is explicit.
class PendingEventStore {
public:
    explicit PendingEventStore(Limits limits = {});
    EnqueueResult add(const scrp::SecurityEvent&, std::size_t byte_charge, Clock::time_point now);
    bool first(scrp::SecurityEvent& result) const;
    bool acknowledge(const std::string& event_id);
    std::vector<std::string> expire(Clock::time_point now);
    std::size_t size() const { return entries_.size(); }
    std::size_t bytes() const { return bytes_; }
private:
    struct Entry { scrp::SecurityEvent event; std::size_t bytes; Clock::time_point added; };
    Limits limits_;
    std::deque<Entry> entries_;
    std::size_t bytes_ = 0;
};
} // namespace telemetry
