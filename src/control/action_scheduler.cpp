// src/control/action_scheduler.cpp
#include "action_scheduler.h"

#include "control_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <sstream>
#include <vector>
#include <stdexcept>

namespace runner::control {

namespace {
using Clock = std::chrono::steady_clock;

std::int64_t elapsed_ms(Clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

bool uses_coordinates(const ActionRequest& r) {
    return r.operation == Operation::MouseMove || r.operation == Operation::MouseClick ||
           (r.operation == Operation::MouseScroll && r.x && r.y);
}

class WindowsBackend final : public WorkerBackend {
public:
    WindowsBackend(InputLimits limits, std::function<ErrorCode()> gate)
        : executor_(limits), gate_(std::move(gate)) {}
    CaptureResult capture(const CaptureLimits& limits) override {
        return capture_primary_display_png(limits);
    }
    void screen_size(int& w, int& h) override { primary_screen_size(w, h); }
    ExecOutcome input(const ActionRequest& r, Clock::time_point deadline,
                      const std::atomic<bool>& cancelled) override {
        switch (r.operation) {
        case Operation::MouseMove: return executor_.move(*r.x, *r.y);
        case Operation::MouseClick: return executor_.click(*r.x, *r.y, r.button, r.click_count);
        case Operation::MouseScroll:
            return executor_.scroll(r.scroll_direction, r.scroll_steps,
                                    r.x ? &*r.x : nullptr, r.y ? &*r.y : nullptr);
        case Operation::KeyboardType: return executor_.type_text(r.text_utf8, deadline, &cancelled, gate_);
        case Operation::KeyboardPress: return executor_.press(r.key);
        case Operation::KeyboardHotkey: return executor_.hotkey(r.modifiers, r.key);
        default: return {ActionStatus::Failed, ErrorCode::InvalidArgument, "invalid operation", 0};
        }
    }
private:
    InputExecutor executor_;
    std::function<ErrorCode()> gate_;
};
void deliver(const ActionScheduler::ResultCallback& cb, const ActionResult& result) noexcept {
    if (cb) { try { cb(result); } catch (...) { /* result remains queryable via state() */ } }
}
}  // namespace

ActionScheduler::ActionScheduler(Config config)
    : config_(std::move(config)),
      backend_(config_.backend ? config_.backend : std::make_shared<WindowsBackend>(config_.input, config_.execution_gate)) {
    if (!config_.generation || !config_.max_queue || config_.max_queue > 32 ||
        !config_.max_records || config_.max_records > 65536 ||
        !config_.max_observations || config_.max_observations > 32 ||
        config_.observation_max_age_ms < 1 || config_.observation_max_age_ms > 10000 ||
        !config_.input.max_text_bytes || config_.input.max_text_bytes > 4096 ||
        config_.input.max_scroll_steps < 1 || config_.input.max_scroll_steps > 10 ||
        config_.input.max_hotkey_keys < 2 || config_.input.max_hotkey_keys > 4 ||
        config_.input.inter_key_delay_ms < 0 || config_.input.inter_key_delay_ms > 10 ||
        !config_.capture.max_png_bytes || config_.capture.max_png_bytes > 8u*1024u*1024u ||
        !config_.capture.max_pixels || config_.capture.max_pixels > 16000000ull)
        throw std::invalid_argument("invalid scheduler limits");
}

ActionScheduler::~ActionScheduler() { stop(); }

void ActionScheduler::start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_ || stopping_) return;
    worker_ = std::thread(&ActionScheduler::worker_loop, this);
    running_ = true;
}

std::size_t ActionScheduler::request_stop() {
    std::size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) dropped = queue_.size();
        stopping_ = true;
        cancelled_.store(true);
        for (const auto& job : queue_) {
            auto& result = records_.at(job.request.action_id).result;
            result.status = ActionStatus::Blocked;
            result.error = ErrorCode::Cancelled;
            result.message = "stopped before start; not executed";
        }
        observations_.clear();
    }
    cv_.notify_all();
    return dropped;
}

void ActionScheduler::stop() {
    request_stop();
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (worker_.joinable()) {
        // Callbacks may request_stop(), but must not destroy/join their own worker.
        if (worker_.get_id() == std::this_thread::get_id()) return;
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

std::string ActionScheduler::fingerprint(const ActionRequest& r) {
    std::ostringstream s;
    const char sep = '\x1f';
    s << static_cast<int>(r.operation) << sep
      << (r.x ? std::to_string(*r.x) : "-") << sep << (r.y ? std::to_string(*r.y) : "-") << sep
      << r.observation_id.size() << ':' << r.observation_id << sep << static_cast<int>(r.button) << sep << r.click_count << sep
      << static_cast<int>(r.scroll_direction) << sep << r.scroll_steps << sep
      << r.text_utf8.size() << ':' << r.text_utf8 << sep << static_cast<int>(r.key) << sep;
    for (Modifier m : r.modifiers) s << static_cast<int>(m) << ',';
    s << sep << r.timeout_ms << sep << r.task_id.size() << ':' << r.task_id
      << sep << r.policy_version.size() << ':' << r.policy_version;
    const auto bytes = s.str();
    auto hash = util::sha256_hex(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    if (hash.empty()) throw std::runtime_error("fingerprint hashing failed");
    return hash;
}

SubmitAck ActionScheduler::submit(const ActionRequest& request, ResultCallback on_result) {
    SubmitAck ack;
    std::string message;
    const ErrorCode arg_error = validate_arguments(request, config_.input, message);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stopping_) {
        ack.error = ErrorCode::NotRunning;
        ack.message = "scheduler is not running";
        return ack;
    }
    if (arg_error != ErrorCode::None) {
        ack.error = arg_error;
        ack.message = message;
        return ack;
    }
    // 1) 중복 검사: 같은 action_id가 있으면 절대 다시 실행하지 않는다.
    auto existing = records_.find(request.action_id);
    if (existing != records_.end()) {
        if (existing->second.fingerprint == fingerprint(request)) {
            ack.duplicate = true;
            ack.existing_status = existing->second.result.status;
            ack.message = "duplicate action_id; not re-executed, use state query";
        } else {
            ack.error = ErrorCode::DuplicateConflict;
            ack.message = "action_id reused with a different payload";
        }
        return ack;
    }
    // 2) 인자·좌표 검증
    if (uses_coordinates(request)) {
        const ErrorCode coord_error = check_coordinates(request, message);
        if (coord_error != ErrorCode::None) {
            ack.error = coord_error;
            ack.message = message;
            return ack;
        }
    }
    // 3) 큐 상한
    if (queue_.size() >= config_.max_queue) {
        ack.error = ErrorCode::QueueFull;
        ack.message = "queue limit reached";
        return ack;
    }

    if (records_.size() >= config_.max_records) {
        ack.error = ErrorCode::RecordLimit;
        ack.message = "action ledger full; Host must create a new generation";
        return ack;
    }

    Record rec;
    rec.fingerprint = fingerprint(request);
    rec.result.action_id = request.action_id;
    rec.result.task_id = request.task_id;
    rec.result.operation = request.operation;
    rec.result.status = ActionStatus::Pending;
    records_.emplace(request.action_id, std::move(rec));

    queue_.push_back(Job{request, std::move(on_result)});
    cv_.notify_one();
    ack.accepted = true;
    return ack;
}

std::optional<ActionResult> ActionScheduler::state(const std::string& action_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = records_.find(action_id);
    if (it == records_.end()) return std::nullopt;
    return it->second.result;
}

std::size_t ActionScheduler::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

ErrorCode ActionScheduler::check_coordinates(const ActionRequest& r, std::string& message) const {
    const ObsEntry* obs = nullptr;
    for (const ObsEntry& e : observations_) {
        if (e.id == r.observation_id) { obs = &e; break; }
    }
    if (!obs) {
        message = "unknown observation_id for this generation";
        return ErrorCode::StaleObservation;
    }
    if (elapsed_ms(obs->captured) >= config_.observation_max_age_ms) {
        message = "observation is older than the allowed age";
        return ErrorCode::StaleObservation;
    }
    int w = 0, h = 0;
    backend_->screen_size(w, h);
    if (w != obs->width || h != obs->height) {
        message = "screen resolution changed since observation";
        return ErrorCode::StaleObservation;
    }
    if (*r.x < 0 || *r.y < 0 || *r.x >= obs->width || *r.y >= obs->height) {
        message = "coordinates outside observation bounds";
        return ErrorCode::OutOfBounds;
    }
    return ErrorCode::None;
}

void ActionScheduler::remember_observation(const Observation& obs, Clock::time_point at) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    observations_.push_back(ObsEntry{obs.observation_id, obs.width, obs.height, at});
    while (observations_.size() > config_.max_observations) observations_.pop_front();
}

void ActionScheduler::worker_loop() {
    // WIC 인코더용 COM. 스레드 종료 시 해제한다.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    for (;;) {
        Job job;
        bool blocked = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) break;
            blocked = stopping_;
            job = std::move(queue_.front());
            queue_.pop_front();
            // 실행 시작 전에 RUNNING을 남긴다. 이후 장애 시 실행 여부는 UNKNOWN으로 해석된다.
            if (!blocked) records_.at(job.request.action_id).result.status = ActionStatus::Running;
        }

        ActionResult result;
        try {
            if (blocked) result = *state(job.request.action_id);
            else result = execute(job.request);
        } catch (...) {
            result.action_id = job.request.action_id;
            result.task_id = job.request.task_id;
            result.operation = job.request.operation;
            result.status = ActionStatus::Unknown;
            result.error = ErrorCode::Internal;
            result.message = "worker exception; execution may have occurred; do not retry";
            request_stop();
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = records_.find(job.request.action_id);
            if (it != records_.end()) {
                it->second.result = result;
                it->second.result.observation.reset();  // 기록에는 PNG 바이트를 보관하지 않는다
            }
        }
        deliver(job.callback, result);
    }

    if (SUCCEEDED(com)) CoUninitialize();
}

ActionResult ActionScheduler::execute(const ActionRequest& r) {
    ActionResult result;
    result.action_id = r.action_id;
    result.task_id = r.task_id;
    result.operation = r.operation;
    const Clock::time_point started = Clock::now();
    const Clock::time_point deadline = started + std::chrono::milliseconds(r.timeout_ms);

    const auto gate = cancelled_.load() ? ErrorCode::Cancelled
        : (config_.execution_gate ? config_.execution_gate() : ErrorCode::None);
    if (gate != ErrorCode::None) {
        result.status = ActionStatus::Blocked;
        result.error = gate;
        result.message = "execution gate closed; no input sent";
        return result;
    }

    // 큐 대기 중 observation이 오래됐을 수 있으므로 실행 직전에 다시 확인한다.
    if (uses_coordinates(r)) {
        std::string message;
        ErrorCode e;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            e = check_coordinates(r, message);
        }
        if (e != ErrorCode::None) {
            result.status = ActionStatus::Failed;
            result.error = e;
            result.message = message + " (no input sent)";
            result.execution_time_ms = elapsed_ms(started);
            return result;
        }
    }

    ExecOutcome out;
    switch (r.operation) {
        case Operation::Observe: {
            CaptureResult cap = backend_->capture(config_.capture);
            const Clock::time_point captured = Clock::now();
            if (!cap.ok) {
                out.status = ActionStatus::Failed;
                out.error = ErrorCode::CaptureFailed;
                out.message = cap.error;
                break;
            }
            if (captured > deadline) {
                out.status = ActionStatus::Failed;
                out.error = ErrorCode::Timeout;
                out.message = "capture exceeded timeout";
                break;
            }
            Observation obs;
            obs.observation_id = "OBS-" + util::random_uuid_v4();
            obs.generation = config_.generation;
            obs.width = cap.width;
            obs.height = cap.height;
            obs.captured_at = util::utc_now_rfc3339();
            obs.sha256_hex = util::sha256_hex(cap.png.data(), cap.png.size());
            obs.png = std::move(cap.png);
            if (obs.sha256_hex.empty() || obs.observation_id.size() <= 4) {
                out.status = ActionStatus::Failed;
                out.error = ErrorCode::Internal;
                out.message = "hash or id generation failed";
                break;
            }
            remember_observation(obs, started);
            result.observation = std::move(obs);
            out.status = ActionStatus::Success;
            out.error = ErrorCode::None;
            break;
        }
        default:
            out = backend_->input(r, deadline, cancelled_);
            break;
    }

    if (out.status == ActionStatus::Success && r.operation != Operation::Observe && Clock::now() >= deadline) {
        out.status = ActionStatus::Unknown;
        out.error = ErrorCode::Timeout;
        out.message = "input returned after deadline; may have executed; do not retry";
    }
    result.status = out.status;
    result.error = out.error;
    result.message = out.message;
    result.units_sent = out.units_sent;
    result.execution_time_ms = elapsed_ms(started);
    return result;
}

}  // namespace runner::control
