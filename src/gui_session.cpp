#include "gui_session.h"
#include <algorithm>
#include <windows.h>
#include <stdexcept>

namespace runner {
namespace {
::control::Reply unsupported(const char* message) {
    ::control::Reply r;
    r.type = "ERROR"; r.status = "ERROR";
    r.error["code"] = "UNSUPPORTED_TYPE";
    r.error["message"] = message;
    r.error["retryable"] = false;
    r.error["recommended_next_step"] = "CHECK_GUI_INTEGRATION";
    return r;
}
::control::Reply ack(bool accepted, std::size_t queued, const std::string& reason) {
    ::control::Reply r;
    r.type = "ACK"; r.status = accepted ? "ACCEPTED" : "REJECTED";
    r.payload["queue_position"] = accepted ? Json::Value(Json::UInt64(queued)) : Json::Value();
    r.payload["reject_reason"] = accepted ? Json::Value() : Json::Value(reason.substr(0,256));
    return r;
}
std::chrono::milliseconds lease(const std::string& stamp) {
    scrp::validate_utc_timestamp(stamp);
    // Schema has already validated the entire UTC string, including its date.
    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(std::stoi(stamp.substr(0,4)));
    utc.wMonth = static_cast<WORD>(std::stoi(stamp.substr(5,2)));
    utc.wDay = static_cast<WORD>(std::stoi(stamp.substr(8,2)));
    utc.wHour = static_cast<WORD>(std::stoi(stamp.substr(11,2)));
    utc.wMinute = static_cast<WORD>(std::stoi(stamp.substr(14,2)));
    utc.wSecond = static_cast<WORD>(std::stoi(stamp.substr(17,2)));
    if (stamp[19] == '.') {
        auto fraction = stamp.substr(20,stamp.size()-21);
        fraction.append(3,'0'); utc.wMilliseconds = static_cast<WORD>(std::stoi(fraction.substr(0,3)));
    }
    FILETIME end{}, now{};
    if (!SystemTimeToFileTime(&utc,&end)) throw std::invalid_argument("Invalid lease date");
    GetSystemTimeAsFileTime(&now);
    auto ticks = [](FILETIME f) { return (std::uint64_t(f.dwHighDateTime)<<32) | f.dwLowDateTime; };
    if (ticks(end) <= ticks(now)) return std::chrono::milliseconds(0);
    return std::chrono::milliseconds(std::min<std::uint64_t>((ticks(end)-ticks(now))/10000,15000));
}
}
GuiSession::GuiSession(runtime::SandboxRuntime::Config config, Authorize authorize, Upload upload)
    : runtime_(config), authorize_(std::move(authorize)), upload_(std::move(upload)),
      schema_(std::make_shared<scrp::HostSenderSchema>(true, config.output_monitor_healthy)) {
    if (!authorize_ || !upload_) throw std::invalid_argument("GUI requires Host authorization and scoped upload adapters");
}
GuiSession::~GuiSession() { stop(); }
control::CaptureResult GuiSession::startup_probe() { return runtime_.startup_probe(); }
runtime::RequestContext GuiSession::request_context(const scrp::Envelope& e) const {
    return {{e.session.session_id,e.session.runtime_id,e.session.generation},e.connection_id.asString()};
}
void GuiSession::connected(const scrp::Envelope& e) {
    auto grant = authorize_(e);
    const auto request = request_context(e);
    if (grant.context.binding.session_id != request.binding.session_id ||
        grant.context.binding.runtime_id != request.binding.runtime_id ||
        grant.context.binding.generation != request.binding.generation ||
        grant.context.connection_id != request.connection_id)
        throw std::invalid_argument("Host grant does not bind the authenticated connection");
    const auto& allowed = schema_->allowed_capabilities();
    auto has = [&](const char* name) { return std::find(allowed.begin(),allowed.end(),Json::Value(name)) != allowed.end(); };
    grant.gui_observe = grant.gui_observe && has("gui.observe");
    grant.gui_input = grant.gui_input && has("gui.input");
    policy_ = grant.policy_version;
    runtime_.activate(grant); // applies the explicitly selected startup authority
    uploader_ = std::thread(&GuiSession::upload_loop,this);
}
control::ActionRequest GuiSession::translate(const scrp::Envelope& e) {
    // Public seam is independently strict, even if called outside Receiver.
    scrp::HostSenderSchema{}.validate_request(e);
    if (e.type != "ACTION_REQUEST") throw std::invalid_argument("Expected ACTION_REQUEST");
    control::ActionRequest r;
    r.action_id = e.action_id.asString(); r.task_id = e.task_id.asString();
    const auto& p = e.payload; const auto& a = p["arguments"];
    r.policy_version = p["policy_version"].asString();
    r.observation_id = p["observation_id"].asString(); r.timeout_ms = p["timeout_ms"].asInt();
    const auto op = p["operation"].asString();
    if (op == "mouse.move" || op == "mouse.click" || op == "mouse.scroll") {
        r.x = a["x"].asInt(); r.y = a["y"].asInt();
        r.operation = op == "mouse.move" ? control::Operation::MouseMove :
                      op == "mouse.click" ? control::Operation::MouseClick : control::Operation::MouseScroll;
        if (op == "mouse.click") {
            r.click_count = a["click_count"].asInt();
            r.button = a["button"] == "left" ? control::MouseButton::Left :
                       a["button"] == "right" ? control::MouseButton::Right : control::MouseButton::Middle;
        }
        if (op == "mouse.scroll") {
            r.scroll_steps = a["steps"].asInt();
            const auto d = a["direction"].asString();
            r.scroll_direction = d == "up" ? control::ScrollDirection::Up : d == "down" ? control::ScrollDirection::Down :
                                 d == "left" ? control::ScrollDirection::Left : control::ScrollDirection::Right;
        }
    } else if (op == "keyboard.type") { r.operation = control::Operation::KeyboardType; r.text_utf8 = a["text"].asString(); }
    else if (op == "keyboard.press") {
        const auto key = control::parse_key(a["key"].asString());
        if (!key) throw std::invalid_argument("Key is not supported by GUI worker");
        r.operation = control::Operation::KeyboardPress; r.key = *key;
    } else if (op == "keyboard.hotkey") {
        r.operation = control::Operation::KeyboardHotkey;
        // The executor supports modifiers followed by exactly one primary key.
        // Reject other orders rather than silently changing the requested chord.
        const auto& keys = a["keys"];
        for (Json::ArrayIndex i=0; i+1<keys.size(); ++i) {
            auto modifier = control::parse_modifier(keys[i].asString());
            if (!modifier) throw std::invalid_argument("Unsupported hotkey modifier or order");
            r.modifiers.push_back(*modifier);
        }
        auto key = control::parse_key(keys[keys.size()-1].asString());
        if (!key) throw std::invalid_argument("Unsupported hotkey primary key");
        r.key = *key;
    } else throw std::invalid_argument("Operation is not supported by GUI worker");
    return r;
}
::control::ReplyHandlers GuiSession::handlers() {
    ::control::ReplyHandlers result;
    for (const auto type : {"OBSERVE","ACTION_REQUEST","HEARTBEAT","STATE_REQUEST","TERMINATE","ARTIFACT_REQUEST"})
        result[type] = [this](const scrp::Envelope& e) { return handle(e); };
    return result;
}
::control::Hooks GuiSession::hooks() {
    return {[this](const scrp::Envelope& e) { connected(e); }, [this] { return poll(); }, [this] { block(); }};
}
::control::Replies GuiSession::handle(const scrp::Envelope& e) {
    if (e.type == "HEARTBEAT" || e.type == "STATE_REQUEST") {
        if (e.type == "HEARTBEAT" && !runtime_.renew_lease(request_context(e),lease(e.payload["lease_expires_at"].asString()))) block();
        const auto s = runtime_.snapshot();
        ::control::Reply r; r.type = e.type == "HEARTBEAT" ? "ALIVE" : "STATE_RESULT";
        r.payload["runtime_state"] = s.lease_valid ? "READY" : "DEGRADED";
        r.payload["worker_alive"] = s.lease_valid && !cancelled_;
        r.payload["queue_depth"] = Json::UInt64(s.queued);
        if (e.type == "HEARTBEAT") r.payload["uptime_ms"] = Json::UInt64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started_).count());
        else {
            r.payload["action_state"] = Json::Value();
            if (!e.payload["action_id"].isNull()) {
                const auto state = runtime_.state(request_context(e),e.payload["action_id"].asString());
                r.payload["action_state"]["action_id"] = e.payload["action_id"];
                r.payload["action_state"]["status"] = state ? control::to_string(state->action.status) : "UNKNOWN";
            }
        }
        return {{std::move(r)}};
    }
    if (e.type == "TERMINATE") {
        if (terminating_) return {{unsupported("Termination already in progress")}};
        terminating_ = true; terminate_ = e;
        dropped_ = block();
        stopper_ = std::thread([this] { runtime_.stop(); if (uploader_.joinable()) uploader_.join(); stopped_ = true; });
        return {{},false,true};
    }
    if (e.type == "ARTIFACT_REQUEST") return {{unsupported("Artifact upload adapter is not connected")}};
    if (cancelled_) {
        if (e.type == "ACTION_REQUEST") return {{ack(false,0,"GUI session blocked")}};
        return {{unsupported("GUI session blocked")}};
    }
    if (runtime_.snapshot().queued >= schema_->queue_limit()) {
        if (e.type == "ACTION_REQUEST") return {{ack(false,0,"Negotiated queue limit reached")}};
        return {{unsupported("Observation queue limit reached")}};
    }
    control::ActionRequest action;
    if (e.type == "OBSERVE") {
        action.action_id = e.action_id.asString(); action.task_id = e.task_id.asString();
        action.policy_version = policy_; action.timeout_ms = 5000;
    } else {
        try { action = translate(e); }
        catch (const std::invalid_argument&) { return {{ack(false,0,"Unsupported GUI operation, key or arguments")}}; }
    }
    const auto submitted = runtime_.submit(request_context(e),action,
        [this,e](const runtime::RuntimeResult& r) { completed(e,r); });
    if (e.type == "OBSERVE") {
        if (!submitted.accepted) return {{unsupported("Observation rejected by GUI runtime")}};
        return {{},false,true};
    }
    return {{ack(submitted.accepted,runtime_.snapshot().queued,submitted.message)},false,submitted.accepted};
}
void GuiSession::enqueue(::control::DeferredReply r) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (results_.size() >= 34) throw std::runtime_error("GUI completion queue full");
        results_.push_back(std::move(r));
    } catch (...) { failed_ = true; block(); }
}
void GuiSession::completed(const scrp::Envelope& e, const runtime::RuntimeResult& r) noexcept {
    try {
        const auto& action = r.action;
        if (action.error == control::ErrorCode::Internal) block();
        if (e.type == "OBSERVE") {
            if (cancelled_ || action.status != control::ActionStatus::Success || !action.observation) {
                enqueue({e.message_id,unsupported("Observation capture failed or cancelled")}); return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_) {
                if (results_.size() >= 34) throw std::runtime_error("GUI completion queue full");
                results_.push_back({e.message_id,unsupported("Observation cancelled")});
                return;
            }
            if (uploads_.size() >= 1 || action.observation->png.size() > 8u*1024u*1024u)
                throw std::runtime_error("Observation upload buffer full");
            uploads_.push_back({e,*action.observation}); cv_.notify_one(); return;
        }
        ::control::Reply reply; reply.type = "ACTION_RESULT";
        reply.status = control::to_string(action.status);
        reply.payload["execution_time_ms"] = Json::Int64(std::max<std::int64_t>(0,action.execution_time_ms));
        reply.payload["result"]["input_delivered"] = action.status == control::ActionStatus::Success;
        reply.payload["result"]["chars_sent"] = Json::UInt64(action.units_sent);
        reply.payload["result"]["detail"] = control::to_string(action.error);
        enqueue({e.message_id,std::move(reply)});
    } catch (...) { failed_ = true; block(); }
}
void GuiSession::upload_loop() {
    for (;;) {
        UploadJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,[this] { return cancelled_ || !uploads_.empty(); });
            if (cancelled_) {
                // The connection can still carry state/termination replies after
                // lease expiry. Complete queued observations without publishing IDs.
                for (const auto& pending : uploads_) {
                    if (results_.size() >= 34) { failed_ = true; break; }
                    results_.push_back({pending.request.message_id,unsupported("Observation cancelled")});
                }
                uploads_.clear(); return;
            }
            job = std::move(uploads_.front()); uploads_.pop_front();
        }
        bool uploaded = false;
        try { uploaded = upload_(job.request,job.observation,cancelled_); } catch (...) {}
        if (cancelled_) {
            enqueue({job.request.message_id,unsupported("Observation cancelled")});
            // Drain any waiting observations on the next loop iteration.
            continue;
        }
        if (!uploaded) {
            enqueue({job.request.message_id,unsupported("Observation upload did not complete")});
            block(); continue;
        }
        const auto& o = job.observation;
        ::control::Reply reply; reply.type = "OBSERVE_RESULT";
        reply.payload["observation_id"] = o.observation_id;
        reply.payload["width"] = o.width; reply.payload["height"] = o.height;
        reply.payload["captured_at"] = o.captured_at; reply.payload["sha256"] = o.sha256_hex;
        reply.payload["upload_id"] = job.request.payload["upload_id"];
        enqueue({job.request.message_id,std::move(reply)});
    }
}
std::vector<::control::DeferredReply> GuiSession::poll() {
    if (failed_) throw std::runtime_error("GUI completion delivery failed");
    if (!runtime_.snapshot().lease_valid) block();
    std::vector<::control::DeferredReply> result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!results_.empty()) { result.push_back(std::move(results_.front())); results_.pop_front(); }
    }
    if (terminate_ && stopped_) {
        ::control::Reply r; r.type = "TERMINATE_RESULT";
        r.payload["worker_stopped"] = true; r.payload["pending_actions_dropped"] = Json::UInt64(dropped_);
        result.push_back({terminate_->message_id,std::move(r),true}); terminate_.reset();
    }
    return result;
}
std::size_t GuiSession::block() noexcept {
    cancelled_ = true; const auto dropped = runtime_.block(); cv_.notify_all(); return dropped;
}
void GuiSession::stop() {
    block();
    if (stopper_.joinable()) stopper_.join();
    else { runtime_.stop(); if (uploader_.joinable()) uploader_.join(); }
}
}
