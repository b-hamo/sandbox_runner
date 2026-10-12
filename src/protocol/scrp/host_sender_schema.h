#pragma once
#include "transport/control/control_receiver.h"
#include <atomic>

namespace scrp {
// Explicit compatibility profile for host_control fed305b (not a frozen SCRP v1).
// One instance per connection. Artifact event contracts remain independent.
class HostSenderSchema : public control::Schema {
public:
    explicit HostSenderSchema(bool gui = false, std::function<bool()> output_monitor = {})
        : gui_(gui), output_monitor_(std::move(output_monitor)) {}
    const Json::Value& allowed_capabilities() const { return allowed_; }
    std::size_t queue_limit() const { return queue_limit_; }
    Json::Value hello(const SessionContext&) const override;
    std::string hello_ack(const Envelope&) const override;
    void validate_request(const Envelope&) const override;
    void validate_reply(const Envelope&) const override;
    std::size_t message_limit() const override { return limit_.load(); }
protected:
    virtual Json::Value capabilities() const {
        Json::Value result(Json::arrayValue);
        if (gui_) { result.append("gui.observe"); result.append("gui.input"); }
        return result;
    }
private:
    bool gui_;
    std::function<bool()> output_monitor_;
    mutable Json::Value allowed_{Json::arrayValue};
    mutable std::size_t queue_limit_ = 1;
    mutable std::atomic<std::size_t> limit_{65536};
};
} // namespace scrp
