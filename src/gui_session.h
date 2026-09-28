#pragma once
#include "runtime/sandbox_runtime.h"
#include "protocol/scrp/host_sender_schema.h"
#include <deque>

namespace runner {
// Trusted in-process wiring, NOT a new SCRP/bootstrap schema. The Host adapter
// supplies verified grants and a scoped uploader; no default authorization.
class GuiSession {
public:
    using Authorize = std::function<runtime::HostGrant(const scrp::Envelope&)>;
    // Runs on a separate uploader thread. Must honor cancellation and a bounded
    // deadline. True means HTTPS upload completed, never merely queued.
    // Implementer binds upload_id/session/task/action/size/expiry to one grant.
    using Upload = std::function<bool(const scrp::Envelope&, const control::Observation&,
                                     const std::atomic<bool>&)>;
    GuiSession(runtime::SandboxRuntime::Config, Authorize, Upload);
    ~GuiSession();
    GuiSession(const GuiSession&) = delete;
    GuiSession& operator=(const GuiSession&) = delete;
    control::CaptureResult startup_probe();
    std::shared_ptr<scrp::HostSenderSchema> schema() const { return schema_; }
    ::control::ReplyHandlers handlers();
    ::control::Hooks hooks();
    std::size_t block() noexcept;
    void stop(); // owner only, after Receiver has stopped
    const runtime::WorkspacePaths& workspace() const { return runtime_.workspace(); }
    static control::ActionRequest translate(const scrp::Envelope&);
private:
    ::control::Replies handle(const scrp::Envelope&);
    void connected(const scrp::Envelope&);
    std::vector<::control::DeferredReply> poll();
    void completed(const scrp::Envelope&, const runtime::RuntimeResult&) noexcept;
    void enqueue(::control::DeferredReply) noexcept;
    void upload_loop();
    runtime::RequestContext request_context(const scrp::Envelope&) const;

    runtime::SandboxRuntime runtime_;
    Authorize authorize_;
    Upload upload_;
    std::shared_ptr<scrp::HostSenderSchema> schema_;
    std::string policy_;
    std::mutex mutex_;
    struct UploadJob { scrp::Envelope request; control::Observation observation; };
    std::deque<UploadJob> uploads_;
    std::deque<::control::DeferredReply> results_;
    std::condition_variable cv_;
    std::atomic<bool> cancelled_{false}, failed_{false}, stopped_{false};
    std::thread uploader_, stopper_;
    std::optional<scrp::Envelope> terminate_;
    std::size_t dropped_ = 0;
    bool terminating_ = false;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};
}
