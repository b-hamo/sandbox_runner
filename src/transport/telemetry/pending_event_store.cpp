#include "pending_event_store.h"
#include <stdexcept>

namespace telemetry {
PendingEventStore::PendingEventStore(Limits limits):limits_(limits) {
    if(!limits.event_bytes || !limits.buffer_bytes || limits.retention.count()<=0)
        throw std::invalid_argument("Invalid Telemetry limits");
}
EnqueueResult PendingEventStore::add(const scrp::SecurityEvent& e,std::size_t bytes,Clock::time_point now) {
    for(const auto& entry:entries_) if(entry.event.event_id==e.event_id)
        return entry.event.payload==e.payload && entry.event.observed_at==e.observed_at ? EnqueueResult::duplicate : EnqueueResult::conflict;
    if(bytes>limits_.event_bytes) return EnqueueResult::event_too_large;
    if(bytes>limits_.buffer_bytes-bytes_) return EnqueueResult::buffer_full;
    entries_.push_back({e,bytes,now}); bytes_+=bytes; return EnqueueResult::accepted;
}
bool PendingEventStore::first(scrp::SecurityEvent& result) const {
    if(entries_.empty()) return false;
    result=entries_.front().event; return true;
}
bool PendingEventStore::acknowledge(const std::string& id) {
    for(auto i=entries_.begin();i!=entries_.end();++i) if(i->event.event_id==id) {
        bytes_-=i->bytes; entries_.erase(i); return true;
    }
    return false;
}
std::vector<std::string> PendingEventStore::expire(Clock::time_point now) {
    std::vector<std::string> expired;
    while(!entries_.empty() && now-entries_.front().added>=limits_.retention) {
        expired.push_back(entries_.front().event.event_id);
        bytes_-=entries_.front().bytes; entries_.pop_front();
    }
    return expired;
}
} // namespace telemetry
