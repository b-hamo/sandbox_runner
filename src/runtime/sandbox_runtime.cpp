#include "sandbox_runtime.h"
#include <stdexcept>

namespace runner::runtime {
using namespace control;
namespace {
bool same(const SessionBinding& a, const SessionBinding& b) {
    return a.session_id == b.session_id && a.runtime_id == b.runtime_id && a.generation == b.generation;
}
bool valid_lease(std::chrono::milliseconds lease) { return lease.count() > 0 && lease.count() <= 15000; }
SubmitAck denied(ErrorCode code, const char* message) {
    SubmitAck result;
    result.error = code;
    result.message = message;
    return result;
}
}
std::int64_t SandboxRuntime::tick_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
SandboxRuntime::SandboxRuntime(Config config) : config_(std::move(config)) {
    if (!config_.worker.backend && !enable_per_monitor_dpi_awareness())
        throw std::runtime_error("per-monitor DPI awareness unavailable");
    workspace_.create(config_.guest_workspace_base, config_.binding.session_id,
                      config_.binding.runtime_id, config_.binding.generation);
    config_.worker.generation = config_.binding.generation;
    config_.worker.execution_gate = [this] { return gate(); };
    worker_ = std::make_unique<ActionScheduler>(config_.worker);
}
SandboxRuntime::~SandboxRuntime() { stop(); }
bool SandboxRuntime::matches(const RequestContext& context) const {
    return activated_ && same(context.binding, config_.binding) &&
        context.connection_id == grant_.context.connection_id;
}
ErrorCode SandboxRuntime::gate() const {
    if (blocked_.load()) return ErrorCode::Cancelled;
    if (tick_ms() >= lease_until_.load()) {
        blocked_.store(true); // renewal after expiration cannot silently resume work
        return ErrorCode::LeaseExpired;
    }
    return ErrorCode::None;
}
CaptureResult SandboxRuntime::startup_probe() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activated_ || stopped_ || blocked_.load()) throw std::logic_error("probe only allowed before activation");
    const auto started = tick_ms();
    auto result = config_.worker.backend ? config_.worker.backend->capture(config_.worker.capture)
                                         : capture_primary_display_png(config_.worker.capture);
    probe_ok_ = result.ok && !result.png.empty() && result.png.size() <= config_.worker.capture.max_png_bytes &&
        result.width > 0 && result.height > 0 &&
        static_cast<std::uint64_t>(result.width) * result.height <= config_.worker.capture.max_pixels &&
        tick_ms() - started < 5000;
    if (!probe_ok_) { result.ok = false; result.error = "startup capture failed or exceeded limits"; result.png.clear(); }
    return result;
}
void SandboxRuntime::activate(const HostGrant& grant) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activated_ || blocked_.load() || stopped_) throw std::logic_error("runtime cannot be reactivated");
    if (!same(grant.context.binding, config_.binding) || grant.context.connection_id.empty() ||
        grant.context.connection_id.size() > 128 || grant.policy_version.empty() || grant.policy_version.size() > 128 ||
        !probe_ok_ || !grant.startup_verified || !grant.network_policy_verified || !grant.required_monitoring_verified ||
        !valid_lease(grant.lease)) throw std::invalid_argument("unverified Host grant or invalid binding/lease");
    grant_ = grant;
    lease_until_.store(tick_ms() + grant.lease.count());
    worker_->start();
    activated_ = true;
}
bool SandboxRuntime::renew_lease(const RequestContext& context, std::chrono::milliseconds lease) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches(context) || !valid_lease(lease) || gate() != ErrorCode::None) return false;
    lease_until_.store(tick_ms() + lease.count());
    return true;
}
SubmitAck SandboxRuntime::submit(const RequestContext& context, const ActionRequest& action, ResultCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches(context)) return denied(ErrorCode::ProtocolDenied, "session/runtime/generation/connection mismatch");
    if (action.task_id.empty() || action.task_id.size() > 128 || action.policy_version != grant_.policy_version)
        return denied(ErrorCode::ProtocolDenied, "task or policy binding mismatch");
    if (const auto error = gate(); error != ErrorCode::None)
        return denied(error, "runtime blocked or lease expired");
    if ((action.operation == Operation::Observe && !grant_.gui_observe) ||
        (action.operation != Operation::Observe && !grant_.gui_input))
        return denied(ErrorCode::CapabilityDenied, "capability not granted by Host");
    const auto binding = config_.binding;
    const auto task = action.task_id;
    return worker_->submit(action, [binding, task, cb = std::move(cb)](const ActionResult& result) {
        if (cb) cb(RuntimeResult{binding, task, result});
    });
}
std::optional<RuntimeResult> SandboxRuntime::state(const RequestContext& context, const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches(context)) return std::nullopt;
    auto result = worker_->state(id);
    if (!result) return std::nullopt;
    return RuntimeResult{config_.binding, result->task_id, std::move(*result)};
}
RuntimeSnapshot SandboxRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool alive = activated_ && gate() == ErrorCode::None;
    const auto state = stopped_ ? RuntimeSnapshot::State::Stopped : blocked_.load() ? RuntimeSnapshot::State::Blocked :
        activated_ ? RuntimeSnapshot::State::Ready : RuntimeSnapshot::State::Prepared;
    return {state, worker_->queued(), alive, alive && grant_.gui_observe, alive && grant_.gui_input};
}
std::size_t SandboxRuntime::block() {
    blocked_.store(true);
    return worker_->request_stop();
}
void SandboxRuntime::stop() {
    block();
    worker_->stop();
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
}
} // namespace runner::runtime
