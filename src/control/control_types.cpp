// src/control/control_types.cpp
#include "control_types.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace runner::control {

const char* to_string(Operation op) {
    switch (op) {
        case Operation::Observe:        return "observe";
        case Operation::MouseMove:      return "mouse.move";
        case Operation::MouseClick:     return "mouse.click";
        case Operation::MouseScroll:    return "mouse.scroll";
        case Operation::KeyboardType:   return "keyboard.type";
        case Operation::KeyboardPress:  return "keyboard.press";
        case Operation::KeyboardHotkey: return "keyboard.hotkey";
    }
    return "unknown";
}

const char* to_string(ActionStatus status) {
    switch (status) {
        case ActionStatus::Pending:        return "PENDING";
        case ActionStatus::Running:        return "RUNNING";
        case ActionStatus::Success:        return "SUCCESS";
        case ActionStatus::PartialSuccess: return "PARTIAL_SUCCESS";
        case ActionStatus::Failed:         return "FAILED";
        case ActionStatus::Unknown:        return "UNKNOWN";
        case ActionStatus::Blocked:        return "BLOCKED";
    }
    return "UNKNOWN";
}

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::None:              return "NONE";
        case ErrorCode::InvalidArgument:   return "INVALID_ARGUMENT";
        case ErrorCode::OutOfBounds:       return "OUT_OF_BOUNDS";
        case ErrorCode::StaleObservation:  return "STALE_OBSERVATION";
        case ErrorCode::DuplicateConflict: return "DUPLICATE_CONFLICT";
        case ErrorCode::QueueFull:         return "QUEUE_FULL";
        case ErrorCode::NotRunning:        return "NOT_RUNNING";
        case ErrorCode::InputRejected:     return "INPUT_REJECTED";
        case ErrorCode::CaptureFailed:     return "CAPTURE_FAILED";
        case ErrorCode::Timeout:           return "ACTION_TIMEOUT";
        case ErrorCode::Cancelled:         return "CANCELLED";
        case ErrorCode::Internal:          return "INTERNAL";
        case ErrorCode::RecordLimit:       return "RECORD_LIMIT";
        case ErrorCode::ProtocolDenied:    return "PROTOCOL_DENIED";
        case ErrorCode::CapabilityDenied:  return "CAPABILITY_DENIED";
        case ErrorCode::LeaseExpired:      return "LEASE_EXPIRED";
    }
    return "INTERNAL";
}

namespace {
std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}
}  // namespace

std::optional<Key> parse_key(const std::string& raw) {
    static const std::pair<const char*, Key> table[] = {
        {"ENTER", Key::Enter}, {"TAB", Key::Tab}, {"ESCAPE", Key::Escape},
        {"BACKSPACE", Key::Backspace}, {"DELETE", Key::Delete}, {"SPACE", Key::Space},
        {"UP", Key::Up}, {"DOWN", Key::Down}, {"LEFT", Key::Left}, {"RIGHT", Key::Right},
        {"HOME", Key::Home}, {"END", Key::End}, {"PAGEUP", Key::PageUp}, {"PAGEDOWN", Key::PageDown},
        {"F1", Key::F1}, {"F2", Key::F2}, {"F3", Key::F3}, {"F4", Key::F4},
        {"F5", Key::F5}, {"F6", Key::F6}, {"F7", Key::F7}, {"F8", Key::F8},
        {"F9", Key::F9}, {"F10", Key::F10}, {"F11", Key::F11}, {"F12", Key::F12},
    };
    const std::string name = upper(raw);
    for (const auto& [text, key] : table) {
        if (name == text) return key;
    }
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
        return static_cast<Key>(static_cast<int>(Key::A) + (name[0] - 'A'));
    }
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
        return static_cast<Key>(static_cast<int>(Key::Num0) + (name[0] - '0'));
    }
    return std::nullopt;
}

std::optional<Modifier> parse_modifier(const std::string& raw) {
    const std::string name = upper(raw);
    if (name == "CTRL" || name == "CONTROL") return Modifier::Ctrl;
    if (name == "SHIFT") return Modifier::Shift;
    if (name == "ALT") return Modifier::Alt;
    return std::nullopt;  // WIN 등은 허용하지 않음
}

}  // namespace runner::control
