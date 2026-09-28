#pragma once
#include "control/action_scheduler.h"
#include "session_workspace.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace runner::runtime {
struct SessionBinding {
    std::string session_id, runtime_id;
    std::uint64_t generation = 0;
};
// Supplied only after E validates TLS, credentials, envelope/replay and schema.
struct RequestContext {
    SessionBinding binding;
    std::string connection_id;
};
struct HostGrant {
    RequestContext context;
    std::string policy_version;
    bool gui_observe = false;
    bool gui_input = false;
    bool startup_verified = false;
    bool network_policy_verified = false; // Host firewall verification, not a Guest claim
    bool required_monitoring_verified = false;
    std::chrono::milliseconds lease{0};  // receiver-local monotonic duration
};
struct RuntimeResult {
    SessionBinding binding;
    std::string task_id;
    control::ActionResult action;
};
struct RuntimeSnapshot {
    enum class State { Prepared, Ready, Blocked, Stopped } state;
    std::size_t queued = 0;
    bool lease_valid = false;
    bool gui_observe = false, gui_input = false;
    // Blocked never means that Sandbox processes/network have been paused.
    const char* freeze_mode = "ACTION_BLOCK_ONLY";
};

class SandboxRuntime {
public:
    enum class StartupAuthority { VerifiedGrant, Host6055 };
    struct Config {
        SessionBinding binding;
        std::filesystem::path guest_workspace_base;
        control::ActionScheduler::Config worker;
        StartupAuthority startup_authority = StartupAuthority::VerifiedGrant;
        // Host6055 uses the authenticated Broker as the input/startup authority.
        // file coverage means a live session Output watcher, not system-wide monitoring.
        std::function<bool()> output_monitor_healthy;
    };
    using ResultCallback = std::function<void(const RuntimeResult&)>;
    explicit SandboxRuntime(Config config);
    ~SandboxRuntime();
    SandboxRuntime(const SandboxRuntime&) = delete;
    SandboxRuntime& operator=(const SandboxRuntime&) = delete;

    // Prepared != Host READY. activate requires explicit trusted Host checks.
    // Before activate, verify capture locally; Host separately validates the uploaded probe.
    // Probe data has no observation_id and cannot authorize coordinate input.
    control::CaptureResult startup_probe();
    static std::vector<std::string> supported_capabilities() { return {"gui.observe", "gui.input"}; }
    void activate(const HostGrant& grant);
    bool renew_lease(const RequestContext&, std::chrono::milliseconds lease);
    control::SubmitAck submit(const RequestContext&, const control::ActionRequest&, ResultCallback);
    std::optional<RuntimeResult> state(const RequestContext&, const std::string& action_id) const;
    RuntimeSnapshot snapshot() const;
    std::size_t block(); // disconnect, monitoring loss, revocation, terminate; no auto-resume
    void stop();  // joins the worker; call from the owner, never a result callback
    const WorkspacePaths& workspace() const { return workspace_.paths(); }
private:
    bool matches(const RequestContext&) const;
    control::ErrorCode gate() const;
    static std::int64_t tick_ms();
    Config config_;
    SessionWorkspace workspace_;
    mutable std::mutex mutex_;
    HostGrant grant_;
    bool activated_ = false, stopped_ = false;
    bool probe_ok_ = false;
    mutable std::atomic<bool> blocked_{false};
    std::atomic<std::int64_t> lease_until_{0};
    std::unique_ptr<control::ActionScheduler> worker_;
};
} // namespace runner::runtime
