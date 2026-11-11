#include "app_shortcuts.h"
#include <cwctype>
#include <string>
#include <vector>

namespace runner::control {
namespace {
unsigned modifiers(const std::vector<Modifier>& values) {
    unsigned bits = 0;
    for (auto value : values) {
        unsigned bit = value == Modifier::Ctrl ? 1 : value == Modifier::Shift ? 2 :
                       value == Modifier::Alt ? 4 : value == Modifier::Win ? 8 : 0;
        if (!bit || (bits & bit)) return 0;
        bits |= bit;
    }
    return bits;
}
std::wstring key_name(Key key) {
    if (key >= Key::A && key <= Key::Z) return std::wstring(1, L'A' + int(key) - int(Key::A));
    if (key >= Key::Num0 && key <= Key::Num9) return std::wstring(1, L'0' + int(key) - int(Key::Num0));
    if (key >= Key::F1 && key <= Key::F12) return L"F" + std::to_wstring(1 + int(key) - int(Key::F1));
    switch (key) {
    case Key::Enter: return L"ENTER"; case Key::Tab: return L"TAB";
    case Key::Escape: return L"ESC"; case Key::Backspace: return L"BACKSPACE";
    case Key::Delete: return L"DEL"; case Key::Space: return L"SPACE";
    case Key::Home: return L"HOME"; case Key::End: return L"END";
    case Key::PageUp: return L"PGUP"; case Key::PageDown: return L"PGDN";
    case Key::Up: return L"UP"; case Key::Down: return L"DOWN";
    case Key::Left: return L"LEFT"; case Key::Right: return L"RIGHT";
    default: return {};
    }
}
bool matches(const wchar_t* label, unsigned bits, const std::wstring& key) {
    const wchar_t* suffix = wcschr(label, L'\t');
    if (!suffix) return false;
    std::wstring normalized;
    for (++suffix; *suffix; ++suffix)
        if (!iswspace(*suffix)) normalized += static_cast<wchar_t>(towupper(*suffix));
    unsigned found = 0;
    size_t start = 0, end;
    while ((end = normalized.find(L'+', start)) != std::wstring::npos) {
        const auto part = normalized.substr(start, end - start);
        unsigned bit = part == L"CTRL" || part == L"CONTROL" ? 1 : part == L"SHIFT" ? 2 :
                       part == L"ALT" ? 4 : part == L"WIN" ? 8 : 0;
        if (!bit || (found & bit)) return false;
        found |= bit; start = end + 1;
    }
    auto last = normalized.substr(start);
    if (last == L"DELETE") last = L"DEL";
    if (last == L"ESCAPE") last = L"ESC";
    if (last == L"RETURN") last = L"ENTER";
    return found == bits && last == key;
}
bool read_item(HMENU menu, UINT index, MENUITEMINFOW& info, wchar_t (&text)[512]) {
    info = {}; info.cbSize = sizeof(info);
    info.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
    info.dwTypeData = text; info.cch = 512;
    return GetMenuItemInfoW(menu, index, TRUE, &info) && info.cch < 511;
}
struct MenuSearch {
    HWND target;
    const std::function<bool()>& valid;
    unsigned bits;
    std::wstring key;
    unsigned visited = 0, matches_found = 0;
    UINT command = 0;
    bool blocked = false, unknown = false;
    bool send(UINT msg, WPARAM wp, LPARAM lp) {
        if (!valid()) { blocked = true; return false; }
        DWORD_PTR answer = 0;
        if (!SendMessageTimeoutW(target, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &answer)) {
            unknown = true; return false;
        }
        return true;
    }
    bool scan(HMENU menu, unsigned depth, bool disabled = false) {
        if (depth > 8 || !valid()) return false;
        const int count = GetMenuItemCount(menu);
        if (count < 0) return false;
        for (int i = 0; i < count; ++i) {
            if (++visited > 512 || !valid()) return false;
            wchar_t text[512]{}; MENUITEMINFOW info{};
            if (!read_item(menu, i, info, text)) return false;
            const bool unavailable = disabled || (info.fState & (MFS_DISABLED | MFS_GRAYED));
            if (info.hSubMenu) {
                if (!send(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(info.hSubMenu), MAKELPARAM(i, FALSE)) ||
                    !scan(info.hSubMenu, depth + 1, unavailable)) return false;
            } else if (!(info.fType & MFT_SEPARATOR) && matches(text, bits, key)) {
                ++matches_found;
                if (unavailable || (info.fType & MFT_OWNERDRAW) || !info.wID || info.wID > 0xffff) blocked = true;
                command = info.wID;
            }
        }
        return true;
    }
};
}
ShortcutDelivery deliver_app_shortcut(HWND target, HWND focus, const ActionRequest& request,
                                     const std::function<bool()>& valid) {
    if (!valid() || !IsWindowEnabled(target) || IsIconic(target)) return ShortcutDelivery::Rejected;
    const unsigned bits = modifiers(request.modifiers);
    const auto key = key_name(request.key);
    if (!bits || key.empty()) return ShortcutDelivery::Unsupported;
    if (HMENU menu = GetMenu(target)) {
        MenuSearch search{target, valid, bits, key};
        if (!search.send(WM_INITMENU, reinterpret_cast<WPARAM>(menu), 0) ||
            !search.scan(menu, 0)) return search.unknown ? ShortcutDelivery::Unknown : ShortcutDelivery::Rejected;
        if (search.blocked || search.matches_found > 1) return ShortcutDelivery::Rejected;
        if (search.matches_found == 1) {
            if (!valid() || !IsWindowEnabled(target)) return ShortcutDelivery::Rejected;
            // Queue the same command an accelerator generates. A Save As handler
            // may run a modal loop: synchronous SendMessage would falsely time out.
            return PostMessageW(target, WM_COMMAND, MAKEWPARAM(search.command, 1), 0) ?
                ShortcutDelivery::Delivered : ShortcutDelivery::Rejected;
        }
    }
    DWORD target_pid = 0, focus_pid = 0;
    GetWindowThreadProcessId(target, &target_pid); GetWindowThreadProcessId(focus, &focus_pid);
    if (!focus || !IsWindow(focus) || target_pid != focus_pid ||
        (focus != target && !IsChild(target, focus)) || !IsWindowEnabled(focus) || bits != 1)
        return ShortcutDelivery::Unsupported;
    wchar_t name[128]{}; GetClassNameW(focus, name, 128);
    // Do not send Edit-specific messages to arbitrary custom controls.
    if (_wcsicmp(name, L"Edit") && _wcsicmp(name, L"RichEdit20W") &&
        _wcsicmp(name, L"RichEdit20A") && _wcsicmp(name, L"RICHEDIT50W")) return ShortcutDelivery::Unsupported;
    UINT msg = 0; WPARAM wp = 0; LPARAM lp = 0;
    switch (request.key) {
    case Key::A: msg = EM_SETSEL; lp = -1; break;
    case Key::C: msg = WM_COPY; break;
    case Key::X: msg = WM_CUT; break;
    case Key::V: msg = WM_PASTE; break;
    case Key::Z: msg = WM_UNDO; break;
    default: return ShortcutDelivery::Unsupported;
    }
    if (!valid()) return ShortcutDelivery::Rejected;
    DWORD_PTR answer = 0;
    return SendMessageTimeoutW(focus, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &answer) ?
        ShortcutDelivery::Delivered : ShortcutDelivery::Unknown;
}
}
