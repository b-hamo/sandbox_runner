// src/control/control_types.h
// Control 실행 계층의 내부 타입.
// SCRP v0.1 설계안 6쪽(초기 Tool·동작 범위)의 operation 집합을 내부 구조로 표현한다.
// wire JSON 필드명·오류 코드는 W2 스키마 동결 전이므로 여기서 확정하지 않는다.
// Control Transport는 수신한 ACTION_REQUEST payload를 이 타입으로 변환해 전달한다.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace runner::control {

// 허용 operation은 닫힌 Enum이다. 동적 함수명·스크립트·셸 명령은 받지 않는다.
enum class Operation {
    Observe,          // OBSERVE
    MouseMove,        // mouse.move
    MouseClick,       // mouse.click
    MouseScroll,      // mouse.scroll
    KeyboardType,     // keyboard.type
    KeyboardPress,    // keyboard.press
    KeyboardHotkey,   // keyboard.hotkey
};

enum class MouseButton { Left, Right, Middle };
enum class ScrollDirection { Up, Down, Left, Right };

// SCRP Action 상태. ACK의 ACCEPTED/REJECTED는 SubmitAck로 따로 표현한다.
enum class ActionStatus {
    Pending,          // 큐 대기(아직 미시작)
    Running,          // 실행 시작 기록을 남긴 뒤
    Success,
    PartialSuccess,
    Failed,
    Unknown,
    Blocked,
};

// 내부 오류 코드. wire 오류 코드와의 대응은 W2 스키마에서 정한다.
enum class ErrorCode {
    None,
    InvalidArgument,     // 타입·범위·길이 위반
    OutOfBounds,         // 좌표가 observation 범위 밖
    StaleObservation,    // 모르는/오래된/해상도가 바뀐 observation
    DuplicateConflict,   // 같은 action_id에 다른 payload
    QueueFull,           // 대기 Action 32개 초과
    NotRunning,          // 스케줄러 중지 상태
    InputRejected,       // SendInput이 입력을 모두 넣지 못함(UIPI·보안 데스크톱 등)
    CaptureFailed,
    Timeout,
    Cancelled,           // 미시작 상태에서 중지됨
    Internal,
    RecordLimit,
    ProtocolDenied,
    CapabilityDenied,
    LeaseExpired,
};

const char* to_string(Operation op);
const char* to_string(ActionStatus status);
const char* to_string(ErrorCode code);

// keyboard.press / hotkey용 고정 Key Enum.
// WIN 키는 Win+R 등 실행 경로를 열 수 있으므로 초기 허용 목록에서 제외한다.
enum class Key {
    Enter, Tab, Escape, Backspace, Delete, Space,
    Up, Down, Left, Right, Home, End, PageUp, PageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
};

enum class Modifier { Ctrl, Shift, Alt };

// 문자열 → Enum 변환. 알 수 없는 이름은 std::nullopt (추측하지 않음).
std::optional<Key> parse_key(const std::string& name);
std::optional<Modifier> parse_modifier(const std::string& name);

struct ActionRequest {
    std::string action_id;           // Host(Broker)가 발급한 ID
    Operation operation = Operation::Observe;

    // 좌표 계열(mouse.move/click, 위치 지정 scroll)
    std::optional<int> x;
    std::optional<int> y;
    std::string observation_id;      // 좌표 기준이 된 관찰 ID

    MouseButton button = MouseButton::Left;
    int click_count = 1;             // 1 또는 2

    ScrollDirection scroll_direction = ScrollDirection::Down;
    int scroll_steps = 1;            // 1..10, 1 step = wheel delta 120

    std::string text_utf8;           // keyboard.type, 최대 4 KiB
    Key key = Key::Enter;            // keyboard.press / hotkey의 주 키
    std::vector<Modifier> modifiers; // keyboard.hotkey

    int timeout_ms = 10000;          // GUI 실행 10초, 관찰은 5초 권장
    std::string task_id;            // trusted adapter supplies envelope task_id
    std::string policy_version;     // validated Host policy revision
};

struct Observation {
    std::string observation_id;
    std::uint64_t generation = 0;
    int width = 0;                   // 원본 픽셀
    int height = 0;
    std::string captured_at;         // UTC RFC 3339
    std::string sha256_hex;          // PNG 바이트의 SHA-256
    std::vector<std::uint8_t> png;   // 업로드 전 메모리 버퍼
};

struct ActionResult {
    std::string action_id;
    std::string task_id;
    Operation operation = Operation::Observe;
    ActionStatus status = ActionStatus::Pending;
    ErrorCode error = ErrorCode::None;
    std::string message;
    std::int64_t execution_time_ms = 0;
    std::optional<Observation> observation;  // Observe 성공 시
    std::size_t units_sent = 0;              // keyboard.type 진행량(UTF-16 단위)
};

struct SubmitAck {
    bool accepted = false;           // ACK ACCEPTED/REJECTED
    bool duplicate = false;          // 같은 action_id·같은 payload 재수신
    ErrorCode error = ErrorCode::None;
    std::string message;
    ActionStatus existing_status = ActionStatus::Pending;  // duplicate일 때 기존 상태
};

}  // namespace runner::control
