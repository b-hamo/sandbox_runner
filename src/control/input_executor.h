// src/control/input_executor.h
// Windows SendInput 기반 GUI 입력 실행기.
// 판단·재계획·정책 결정은 하지 않는다. 검증된 요청을 그대로 실행하고 결과만 보고한다.
// 입력 전달 성공은 사용자 목표 달성(예: 파일 다운로드 완료)을 의미하지 않는다.
#pragma once

#include "control_types.h"

#include <chrono>
#include <atomic>
#include <functional>
#include <string>

namespace runner::control {

struct InputLimits {
    std::size_t max_text_bytes = 4096;  // keyboard.type UTF-8 4 KiB
    int max_scroll_steps = 10;
    std::size_t max_hotkey_keys = 4;    // modifier + key 합계
    int inter_key_delay_ms = 2;         // 문자 사이 지연(대상 앱 입력 큐 보호)
};

struct ExecOutcome {
    ActionStatus status = ActionStatus::Failed;
    ErrorCode error = ErrorCode::Internal;
    std::string message;
    std::size_t units_sent = 0;
};

// 좌표를 제외한 인자 검증(타입·범위·길이·UTF-8). 좌표 범위는 스케줄러가 observation과 대조한다.
ErrorCode validate_arguments(const ActionRequest& req, const InputLimits& limits,
                             std::string& message);

class InputExecutor {
public:
    explicit InputExecutor(InputLimits limits = {}) : limits_(limits) {}

    ExecOutcome move(int x, int y);
    ExecOutcome click(int x, int y, MouseButton button, int click_count);
    ExecOutcome scroll(ScrollDirection dir, int steps, const int* x, const int* y);
    ExecOutcome type_text(const std::string& utf8, std::chrono::steady_clock::time_point deadline,
                          const std::atomic<bool>* cancelled = nullptr,
                          const std::function<ErrorCode()>& gate = {});
    ExecOutcome press(Key key);
    ExecOutcome hotkey(const std::vector<Modifier>& modifiers, Key key);

    const InputLimits& limits() const { return limits_; }

private:
    InputLimits limits_;
};

}  // namespace runner::control
