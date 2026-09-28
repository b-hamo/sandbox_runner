// src/control/action_scheduler.h
// Runtime 1개(= Session/Runtime/Generation 1개)에 대응하는 직렬 GUI 실행 스케줄러.
// SCRP v0.1 6~7쪽 규칙을 내부에서 적용한다.
//  - 관찰과 입력을 같은 worker에서 순서대로 실행한다(한 번에 하나).
//  - 대기 Action은 최대 32개.
//  - 좌표 입력은 현재 Generation의 observation_id와 대조하고, 10초 초과·해상도 변경 시 STALE_OBSERVATION.
//  - (action_id) 실행 기록을 유지한다. 같은 ID·같은 payload는 재실행하지 않고, 다른 payload는 거부한다.
//  - 실행 직전에 RUNNING을 기록한다. 자동 재시도·재전송은 하지 않는다.
//  - stop() 시 미시작 Action은 실행하지 않고 BLOCKED(CANCELLED)로 종료한다.
// Heartbeat·상태 조회·종료 처리는 이 worker를 거치지 않는다(state()는 잠금만 사용).
// Generation이 바뀌면 이 객체를 새로 만든다. 이전 큐·observation·기록을 재사용하지 않는다.
#pragma once

#include "control_types.h"
#include "input_executor.h"
#include "screen_capture.h"

#include <chrono>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace runner::control {

// Internal dependency seam; never selected by a wire payload.
class WorkerBackend {
public:
    virtual ~WorkerBackend() = default;
    virtual CaptureResult capture(const CaptureLimits&) = 0;
    virtual void screen_size(int& width, int& height) = 0;
    virtual ExecOutcome input(const ActionRequest&, std::chrono::steady_clock::time_point,
                              const std::atomic<bool>& cancelled) = 0;
};

class ActionScheduler {
public:
    struct Config {
        std::uint64_t generation = 1;
        std::size_t max_queue = 32;
        int observation_max_age_ms = 10000;
        std::size_t max_observations = 8;   // 최근 관찰만 좌표 기준으로 허용
        std::size_t max_records = 4096;     // never evict: reject new IDs at capacity
        InputLimits input;
        CaptureLimits capture;
        std::shared_ptr<WorkerBackend> backend; // null = Windows desktop backend
        // Called immediately before execution. Must be fast, nonblocking, noexcept.
        std::function<ErrorCode()> execution_gate;
    };

    // Callbacks run on the worker. Only enqueue results into a bounded transport queue.
    // Never block on I/O, call stop(), or destroy this object from a callback.
    // request_stop() is safe from a callback. Callback exceptions are isolated.
    using ResultCallback = std::function<void(const ActionResult&)>;

    explicit ActionScheduler(Config config);
    ~ActionScheduler();
    ActionScheduler(const ActionScheduler&) = delete;
    ActionScheduler& operator=(const ActionScheduler&) = delete;

    void start();
    void stop();
    // Nonblocking: closes admission and cooperatively cancels current input.
    // Host must kill the Runner process for a stuck OS call / emergency stop.
    std::size_t request_stop(); // number of queued jobs newly cancelled

    // ACK에 해당. accepted=true는 접수 완료일 뿐 실행 성공이 아니다.
    SubmitAck submit(const ActionRequest& request, ResultCallback on_result);

    // STATE_REQUEST에 해당. 모르는 action_id면 std::nullopt.
    std::optional<ActionResult> state(const std::string& action_id) const;

    std::size_t queued() const;
    std::uint64_t generation() const { return config_.generation; }

private:
    struct Job {
        ActionRequest request;
        ResultCallback callback;
    };
    struct Record {
        std::string fingerprint;
        ActionResult result;
    };
    struct ObsEntry {
        std::string id;
        int width = 0;
        int height = 0;
        std::chrono::steady_clock::time_point captured;
    };

    void worker_loop();
    ActionResult execute(const ActionRequest& request);
    ErrorCode check_coordinates(const ActionRequest& request, std::string& message) const;  // mutex_ 보유 상태
    void remember_observation(const Observation& obs, std::chrono::steady_clock::time_point at);
    static std::string fingerprint(const ActionRequest& request);

    Config config_;
    std::shared_ptr<WorkerBackend> backend_;
    std::atomic<bool> cancelled_{false};
    std::mutex lifecycle_mutex_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::unordered_map<std::string, Record> records_;
    std::deque<ObsEntry> observations_;
    bool running_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace runner::control
