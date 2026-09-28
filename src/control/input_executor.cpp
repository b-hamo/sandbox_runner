// src/control/input_executor.cpp
#include "input_executor.h"

#include "control_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <thread>
#include <vector>

namespace runner::control {

namespace {

struct VkInfo {
    WORD vk;
    bool extended;
};

VkInfo vk_of(Key key) {
    const int k = static_cast<int>(key);
    if (k >= static_cast<int>(Key::A) && k <= static_cast<int>(Key::Z)) {
        return {static_cast<WORD>('A' + (k - static_cast<int>(Key::A))), false};
    }
    if (k >= static_cast<int>(Key::Num0) && k <= static_cast<int>(Key::Num9)) {
        return {static_cast<WORD>('0' + (k - static_cast<int>(Key::Num0))), false};
    }
    if (k >= static_cast<int>(Key::F1) && k <= static_cast<int>(Key::F12)) {
        return {static_cast<WORD>(VK_F1 + (k - static_cast<int>(Key::F1))), false};
    }
    switch (key) {
        case Key::Enter:     return {VK_RETURN, false};
        case Key::Tab:       return {VK_TAB, false};
        case Key::Escape:    return {VK_ESCAPE, false};
        case Key::Backspace: return {VK_BACK, false};
        case Key::Delete:    return {VK_DELETE, true};
        case Key::Space:     return {VK_SPACE, false};
        case Key::Up:        return {VK_UP, true};
        case Key::Down:      return {VK_DOWN, true};
        case Key::Left:      return {VK_LEFT, true};
        case Key::Right:     return {VK_RIGHT, true};
        case Key::Home:      return {VK_HOME, true};
        case Key::End:       return {VK_END, true};
        case Key::PageUp:    return {VK_PRIOR, true};
        case Key::PageDown:  return {VK_NEXT, true};
        default:             return {0, false};
    }
}

WORD vk_of(Modifier m) {
    switch (m) {
        case Modifier::Ctrl:  return VK_LCONTROL;
        case Modifier::Shift: return VK_LSHIFT;
        case Modifier::Alt:   return VK_LMENU;
    }
    return 0;
}

INPUT key_input(WORD vk, bool extended, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    return in;
}

INPUT unicode_input(wchar_t unit, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = static_cast<WORD>(unit);
    in.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
    return in;
}

thread_local bool partial_send = false;
thread_local DWORD send_error = 0;
bool send_all(std::vector<INPUT>& inputs) {
    partial_send = false;
    if (inputs.empty()) return true;
    const UINT n = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    send_error = GetLastError();
    partial_send = n > 0 && n < inputs.size();
    if (partial_send) {
        // Best-effort release only; never replay an action after a partial send.
        for (UINT i = 0; i < n; ++i) {
            INPUT release = inputs[i];
            if (release.type == INPUT_KEYBOARD && !(release.ki.dwFlags & KEYEVENTF_KEYUP)) {
                release.ki.dwFlags |= KEYEVENTF_KEYUP;
            } else if (release.type == INPUT_MOUSE && (release.mi.dwFlags & MOUSEEVENTF_LEFTDOWN)) {
                release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            } else if (release.type == INPUT_MOUSE && (release.mi.dwFlags & MOUSEEVENTF_RIGHTDOWN)) {
                release.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            } else if (release.type == INPUT_MOUSE && (release.mi.dwFlags & MOUSEEVENTF_MIDDLEDOWN)) {
                release.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
            } else { continue; }
            SendInput(1, &release, sizeof(INPUT));
        }
    }
    return n == inputs.size();
}

ExecOutcome ok_outcome() {
    ExecOutcome o;
    o.status = ActionStatus::Success;
    o.error = ErrorCode::None;
    return o;
}

ExecOutcome fail(ErrorCode code, std::string msg) {
    ExecOutcome o;
    o.status = ActionStatus::Failed;
    o.error = code;
    o.message = std::move(msg);
    return o;
}

ExecOutcome rejected_input(const char* what) {
    // SendInput은 UIPI(상위 무결성 창), 보안 데스크톱(UAC·잠금 화면)에서 입력을 넣지 못한다.
    char buf[160];
    wsprintfA(buf, "SendInput rejected %s (GetLastError=%lu)", what,
              static_cast<unsigned long>(send_error));
    auto result = fail(ErrorCode::InputRejected, buf);
    if (partial_send) result.status = ActionStatus::Unknown;
    return result;
}

}  // namespace

ErrorCode validate_arguments(const ActionRequest& req, const InputLimits& limits,
                             std::string& message) {
    if (req.action_id.empty() || req.action_id.size() > 128) {
        message = "action_id must be 1..128 chars";
        return ErrorCode::InvalidArgument;
    }
    if (req.timeout_ms < 1 || req.timeout_ms > 60000) {
        message = "timeout_ms must be 1..60000";
        return ErrorCode::InvalidArgument;
    }
    if (req.observation_id.size() > 128 || req.task_id.size() > 128 || req.policy_version.size() > 128 ||
        static_cast<int>(req.button) < 0 || static_cast<int>(req.button) > static_cast<int>(MouseButton::Middle) ||
        static_cast<int>(req.scroll_direction) < 0 || static_cast<int>(req.scroll_direction) > static_cast<int>(ScrollDirection::Right) ||
        vk_of(req.key).vk == 0 || req.text_utf8.size() > limits.max_text_bytes ||
        req.modifiers.size() > 3) {
        message = "invalid enum or oversized field";
        return ErrorCode::InvalidArgument;
    }
    for (const auto modifier : req.modifiers) {
        if (!vk_of(modifier)) { message = "invalid modifier"; return ErrorCode::InvalidArgument; }
    }
    switch (req.operation) {
        case Operation::Observe:
            return ErrorCode::None;
        case Operation::MouseMove:
        case Operation::MouseClick:
            if (!req.x || !req.y) { message = "x and y are required"; return ErrorCode::InvalidArgument; }
            if (req.observation_id.empty()) {
                message = "observation_id is required for coordinates";
                return ErrorCode::InvalidArgument;
            }
            if (req.operation == Operation::MouseClick &&
                req.click_count != 1 && req.click_count != 2) {
                message = "click_count must be 1 or 2";
                return ErrorCode::InvalidArgument;
            }
            return ErrorCode::None;
        case Operation::MouseScroll:
            if (req.scroll_steps < 1 || req.scroll_steps > limits.max_scroll_steps) {
                message = "scroll steps must be 1..10";
                return ErrorCode::InvalidArgument;
            }
            if (req.x.has_value() != req.y.has_value()) {
                message = "scroll position needs both x and y";
                return ErrorCode::InvalidArgument;
            }
            if (req.x && req.observation_id.empty()) {
                message = "observation_id is required for coordinates";
                return ErrorCode::InvalidArgument;
            }
            return ErrorCode::None;
        case Operation::KeyboardType: {
            if (req.text_utf8.empty() || req.text_utf8.size() > limits.max_text_bytes) {
                message = "text must be 1..4096 UTF-8 bytes";
                return ErrorCode::InvalidArgument;
            }
            auto wide = util::utf8_to_wide(req.text_utf8);
            if (!wide) { message = "text is not valid UTF-8"; return ErrorCode::InvalidArgument; }
            for (wchar_t c : *wide) {
                // 제어 문자는 줄바꿈·탭만 허용한다(ESC 등 기능 키는 keyboard.press로만).
                if (c < 0x20 && c != L'\n' && c != L'\r' && c != L'\t') {
                    message = "text contains a disallowed control character";
                    return ErrorCode::InvalidArgument;
                }
            }
            return ErrorCode::None;
        }
        case Operation::KeyboardPress:
            return ErrorCode::None;  // Key는 이미 닫힌 Enum
        case Operation::KeyboardHotkey: {
            if (req.modifiers.empty()) { message = "hotkey needs a modifier"; return ErrorCode::InvalidArgument; }
            if (req.modifiers.size() + 1 > limits.max_hotkey_keys) {
                message = "hotkey allows at most 4 keys";
                return ErrorCode::InvalidArgument;
            }
            for (std::size_t i = 0; i < req.modifiers.size(); ++i) {
                for (std::size_t j = i + 1; j < req.modifiers.size(); ++j) {
                    if (req.modifiers[i] == req.modifiers[j]) {
                        message = "duplicate modifier";
                        return ErrorCode::InvalidArgument;
                    }
                }
            }
            return ErrorCode::None;
        }
    }
    message = "unknown operation";
    return ErrorCode::InvalidArgument;
}

ExecOutcome InputExecutor::move(int x, int y) {
    if (!SetCursorPos(x, y)) return fail(ErrorCode::InputRejected, "SetCursorPos failed");
    POINT p{};
    if (!GetCursorPos(&p)) return fail(ErrorCode::Internal, "GetCursorPos failed");
    if (p.x != x || p.y != y) {
        // 커서 제한(ClipCursor)이나 DPI 불일치가 있으면 요청 좌표와 달라진다.
        char buf[128];
        wsprintfA(buf, "cursor at (%ld,%ld), requested (%d,%d)", p.x, p.y, x, y);
        return fail(ErrorCode::InputRejected, buf);
    }
    return ok_outcome();
}

ExecOutcome InputExecutor::click(int x, int y, MouseButton button, int click_count) {
    ExecOutcome moved = move(x, y);
    if (moved.status != ActionStatus::Success) return moved;

    DWORD down = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
    if (button == MouseButton::Right) { down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; }
    if (button == MouseButton::Middle) { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }

    std::vector<INPUT> inputs;
    for (int i = 0; i < click_count; ++i) {
        INPUT d{}; d.type = INPUT_MOUSE; d.mi.dwFlags = down;
        INPUT u{}; u.type = INPUT_MOUSE; u.mi.dwFlags = up;
        inputs.push_back(d);
        inputs.push_back(u);
    }
    if (!send_all(inputs)) return rejected_input("mouse click");
    return ok_outcome();
}

ExecOutcome InputExecutor::scroll(ScrollDirection dir, int steps, const int* x, const int* y) {
    if (x && y) {
        ExecOutcome moved = move(*x, *y);
        if (moved.status != ActionStatus::Success) return moved;
    }
    INPUT in{};
    in.type = INPUT_MOUSE;
    const int delta = WHEEL_DELTA * steps;  // 1 step = 120
    switch (dir) {
        case ScrollDirection::Up:    in.mi.dwFlags = MOUSEEVENTF_WHEEL;  in.mi.mouseData = static_cast<DWORD>(delta); break;
        case ScrollDirection::Down:  in.mi.dwFlags = MOUSEEVENTF_WHEEL;  in.mi.mouseData = static_cast<DWORD>(-delta); break;
        case ScrollDirection::Right: in.mi.dwFlags = MOUSEEVENTF_HWHEEL; in.mi.mouseData = static_cast<DWORD>(delta); break;
        case ScrollDirection::Left:  in.mi.dwFlags = MOUSEEVENTF_HWHEEL; in.mi.mouseData = static_cast<DWORD>(-delta); break;
    }
    std::vector<INPUT> inputs{in};
    if (!send_all(inputs)) return rejected_input("mouse wheel");
    return ok_outcome();
}

ExecOutcome InputExecutor::type_text(const std::string& utf8,
                                     std::chrono::steady_clock::time_point deadline,
                                     const std::atomic<bool>* cancelled,
                                     const std::function<ErrorCode()>& gate) {
    auto wide = util::utf8_to_wide(utf8);
    if (!wide) return fail(ErrorCode::InvalidArgument, "text is not valid UTF-8");
    const std::wstring& w = *wide;

    ExecOutcome out = ok_outcome();
    std::size_t i = 0;
    while (i < w.size()) {
        const auto gate_error = (cancelled && cancelled->load()) ? ErrorCode::Cancelled
            : (gate ? gate() : ErrorCode::None);
        if (gate_error != ErrorCode::None) {
            out.status = out.units_sent ? ActionStatus::PartialSuccess : ActionStatus::Blocked;
            out.error = gate_error;
            out.message = "typing cancelled or lease expired";
            return out;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            out.status = out.units_sent > 0 ? ActionStatus::PartialSuccess : ActionStatus::Failed;
            out.error = ErrorCode::Timeout;
            out.message = "deadline reached while typing";
            return out;
        }
        std::vector<INPUT> inputs;
        std::size_t consumed = 1;
        const wchar_t c = w[i];
        if (c == L'\r' || c == L'\n') {
            if (c == L'\r' && i + 1 < w.size() && w[i + 1] == L'\n') consumed = 2;  // CRLF = Enter 1회
            inputs.push_back(key_input(VK_RETURN, false, false));
            inputs.push_back(key_input(VK_RETURN, false, true));
        } else if (c == L'\t') {
            inputs.push_back(key_input(VK_TAB, false, false));
            inputs.push_back(key_input(VK_TAB, false, true));
        } else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size()) {
            // 서로게이트 쌍은 나누어 보내지 않는다.
            consumed = 2;
            inputs.push_back(unicode_input(c, false));
            inputs.push_back(unicode_input(c, true));
            inputs.push_back(unicode_input(w[i + 1], false));
            inputs.push_back(unicode_input(w[i + 1], true));
        } else {
            inputs.push_back(unicode_input(c, false));
            inputs.push_back(unicode_input(c, true));
        }
        if (!send_all(inputs)) {
            ExecOutcome r = rejected_input("text");
            r.units_sent = out.units_sent;
            if (out.units_sent > 0 && r.status != ActionStatus::Unknown) r.status = ActionStatus::PartialSuccess;
            return r;
        }
        out.units_sent += consumed;
        i += consumed;
        if (limits_.inter_key_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(limits_.inter_key_delay_ms));
        }
    }
    return out;
}

ExecOutcome InputExecutor::press(Key key) {
    const VkInfo info = vk_of(key);
    if (info.vk == 0) return fail(ErrorCode::InvalidArgument, "unsupported key");
    std::vector<INPUT> inputs{key_input(info.vk, info.extended, false),
                              key_input(info.vk, info.extended, true)};
    if (!send_all(inputs)) return rejected_input("key press");
    return ok_outcome();
}

ExecOutcome InputExecutor::hotkey(const std::vector<Modifier>& modifiers, Key key) {
    const VkInfo info = vk_of(key);
    if (info.vk == 0) return fail(ErrorCode::InvalidArgument, "unsupported key");

    std::vector<WORD> pressed;
    ExecOutcome result = ok_outcome();
    for (Modifier m : modifiers) {
        std::vector<INPUT> one{key_input(vk_of(m), false, false)};
        if (!send_all(one)) { result = rejected_input("modifier down"); break; }
        pressed.push_back(vk_of(m));
    }
    if (result.status == ActionStatus::Success) {
        std::vector<INPUT> k{key_input(info.vk, info.extended, false),
                             key_input(info.vk, info.extended, true)};
        if (!send_all(k)) result = rejected_input("hotkey key");
    }
    // 실패 여부와 관계없이 눌린 modifier는 역순으로 반드시 해제한다(키 고착 방지).
    for (auto it = pressed.rbegin(); it != pressed.rend(); ++it) {
        std::vector<INPUT> one{key_input(*it, false, true)};
        if (!send_all(one)) {
            result = rejected_input("modifier release");
            result.status = ActionStatus::Unknown;
        }
    }
    return result;
}

}  // namespace runner::control
