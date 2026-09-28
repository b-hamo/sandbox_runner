#pragma once
#include "transport/control/control_receiver.h"
#include <atomic>

namespace scrp {
// Explicit compatibility profile for host_control fed305b (not a frozen SCRP v1).
// One instance per connection. Artifact event contracts remain independent.
class HostSenderSchema : public control::Schema {
public:
    Json::Value hello(const SessionContext&) const override;
    std::string hello_ack(const Envelope&) const override;
    void validate_request(const Envelope&) const override;
    void validate_reply(const Envelope&) const override;
    std::size_t message_limit() const override { return limit_.load(); }
protected:
    // Product has no GUI worker. A future implementation advertises only real capabilities.
    virtual Json::Value capabilities() const { return Json::Value(Json::arrayValue); }
private:
    mutable std::atomic<std::size_t> limit_{65536};
};
} // namespace scrp
