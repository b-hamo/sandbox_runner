// Run in the SCRP Guest. Exercises native menu routing and Edit behavior.
#include "control/app_shortcuts.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace runner::control;
namespace {
unsigned commands = 0, last_command = 0, notifications = 0, init_count = 0;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_COMMAND && !lp) { ++commands; last_command = LOWORD(wp); notifications = HIWORD(wp); return 0; }
    if (m == WM_INITMENUPOPUP) { ++init_count; return 0; }
    return DefWindowProcW(w, m, wp, lp);
}
void pump() { MSG m{}; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m); }
ShortcutDelivery hotkey(HWND w, HWND edit, Key key, std::vector<Modifier> mods, bool valid = true) {
    ActionRequest r; r.operation = Operation::KeyboardHotkey; r.key = key; r.modifiers = std::move(mods);
    return deliver_app_shortcut(w, edit, r, [=] { return valid && IsWindow(w); });
}
std::wstring content(HWND edit) { wchar_t text[256]{}; GetWindowTextW(edit, text, 256); return text; }
}
int main() {
    try {
        WNDCLASSW type{}; type.lpfnWndProc = proc; type.lpszClassName = L"ScrpShortcutFixture";
        type.hInstance = GetModuleHandleW(nullptr);
        check(RegisterClassW(&type) != 0, "register window");
        HMENU menu = CreateMenu(), file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, 701, L"Localized save label\tCtrl+S");
        AppendMenuW(file, MF_STRING, 702, L"Save as\tShift+Ctrl+S");
        AppendMenuW(file, MF_STRING | MF_GRAYED, 703, L"Disabled open\tCtrl+O");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
        HWND w = CreateWindowW(type.lpszClassName, L"Shortcut regression", WS_OVERLAPPEDWINDOW,
                               0, 0, 400, 200, nullptr, menu, type.hInstance, nullptr);
        check(w != nullptr, "window creation");
        HWND edit = CreateWindowW(L"EDIT", L"payload", WS_CHILD | ES_MULTILINE,
                                  0, 0, 200, 100, w, nullptr, type.hInstance, nullptr);
        check(edit != nullptr, "edit creation");
        check(hotkey(w, edit, Key::S, {Modifier::Ctrl}) == ShortcutDelivery::Delivered, "Ctrl+S dispatch");
        pump(); check(commands == 1 && last_command == 701 && notifications == 1 && init_count > 0, "save command identity");
        check(hotkey(w, edit, Key::S, {Modifier::Ctrl, Modifier::Shift}) == ShortcutDelivery::Delivered, "Ctrl+Shift+S dispatch");
        pump(); check(commands == 2 && last_command == 702, "shift must not be lost");
        check(hotkey(w, edit, Key::S, {Modifier::Alt}) == ShortcutDelivery::Unsupported, "Alt must not become Ctrl");
        check(hotkey(w, edit, Key::S, {Modifier::Ctrl, Modifier::Ctrl}) == ShortcutDelivery::Unsupported, "duplicate modifiers");
        check(hotkey(w, edit, Key::O, {Modifier::Ctrl}) == ShortcutDelivery::Rejected, "disabled menu");
        AppendMenuW(file, MF_STRING, 704, L"Duplicate\tCtrl+S");
        check(hotkey(w, edit, Key::S, {Modifier::Ctrl}) == ShortcutDelivery::Rejected, "ambiguous shortcut");
        DeleteMenu(file, 704, MF_BYCOMMAND);
        check(hotkey(w, edit, Key::S, {Modifier::Ctrl}, false) == ShortcutDelivery::Rejected, "closed gate");
        EnableWindow(w, FALSE);
        check(hotkey(w, edit, Key::S, {Modifier::Ctrl}) == ShortcutDelivery::Rejected, "disabled owner while modal");
        EnableWindow(w, TRUE);
        pump(); check(commands == 2, "rejected keys must not send commands");
        check(hotkey(w, edit, Key::A, {Modifier::Ctrl}) == ShortcutDelivery::Delivered, "Ctrl+A");
        DWORD begin = 0, end = 0; SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
        check(begin == 0 && end == 7, "selection range");
        check(hotkey(w, edit, Key::C, {Modifier::Ctrl}) == ShortcutDelivery::Delivered, "Ctrl+C");
        check(hotkey(w, edit, Key::X, {Modifier::Ctrl}) == ShortcutDelivery::Delivered && content(edit).empty(), "Ctrl+X");
        check(hotkey(w, edit, Key::V, {Modifier::Ctrl}) == ShortcutDelivery::Delivered && content(edit) == L"payload", "Ctrl+V clipboard content");
        check(hotkey(w, edit, Key::Z, {Modifier::Ctrl}) == ShortcutDelivery::Delivered && content(edit).empty(), "Ctrl+Z undo paste");
        HWND foreign = CreateWindowW(L"EDIT", L"foreign", WS_POPUP, 0, 0, 100, 40, nullptr, nullptr, type.hInstance, nullptr);
        check(hotkey(w, foreign, Key::A, {Modifier::Ctrl}) == ShortcutDelivery::Unsupported, "foreign focus");
        HWND custom = CreateWindowW(L"STATIC", L"custom", WS_CHILD, 0, 0, 100, 40, w, nullptr, type.hInstance, nullptr);
        check(hotkey(w, custom, Key::A, {Modifier::Ctrl}) == ShortcutDelivery::Unsupported, "custom control");
        DestroyWindow(foreign); DestroyWindow(w);
        std::cout << "PASS: native Ctrl+S, exact modifiers, disabled/ambiguous commands, gate, modal owner, Edit A/C/X/V/Z, foreign/custom focus\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
