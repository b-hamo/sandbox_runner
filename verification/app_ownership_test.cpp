// Run only inside the SCRP Guest. This kills only the creator process it starts,
// never the real Runner. It exercises app launch, crash, Job reopen and input.
#include "control/app_window_backend.h"
#include "control/app_job_keeper.h"
#include "control/control_util.h"
#include "protocol/scrp/envelope.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>

using namespace runner::control;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Handle {
    HANDLE h = nullptr;
    ~Handle() { if (h) CloseHandle(h); }
};
std::wstring self_path() {
    wchar_t path[32768]{};
    check(GetModuleFileNameW(nullptr, path, 32768) != 0, "self path");
    return path;
}
void input(const std::shared_ptr<WorkerBackend>& backend, ActionRequest request) {
    std::atomic<bool> cancelled{false};
    auto result = backend->input(request, std::chrono::steady_clock::now() + std::chrono::seconds(5), cancelled);
    check(result.status == ActionStatus::Success, "backend input rejected");
}
LRESULT CALLBACK app_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_CREATE) {
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"before-crash|", WS_CHILD | WS_VISIBLE,
            10, 10, 280, 40, window, reinterpret_cast<HMENU>(101), GetModuleHandleW(nullptr), nullptr);
        SetFocus(edit); SendMessageW(edit, EM_SETSEL, -1, -1);
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wp, lp);
}
int app(const std::wstring& session) {
    WNDCLASSW type{}; type.lpfnWndProc = app_proc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"ScrpOwnershipFixture"; type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&type);
    HWND window = CreateWindowW(type.lpszClassName, session.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        20, 20, 320, 160, nullptr, nullptr, type.hInstance, nullptr);
    check(window != nullptr, "fixture window");
    Handle ready{OpenEventW(EVENT_MODIFY_STATE, FALSE, (L"Local\\SCRP-Test-" + session).c_str())};
    check(ready.h && SetEvent(ready.h), "fixture ready");
    // A failed test cannot leave this fixture application running indefinitely.
    SetTimer(window, 1, 60000, [](HWND target, UINT, UINT_PTR, DWORD) { PostMessageW(target, WM_CLOSE, 0, 0); });
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
int creator(const std::wstring& session) {
    auto backend = make_app_window_backend(util::wide_to_utf8(session));
    ActionRequest request;
    request.operation = Operation::KeyboardHotkey; request.key = Key::R; request.modifiers = {Modifier::Win};
    input(backend, request);
    request = {}; request.operation = Operation::KeyboardType;
    request.text_utf8 = util::wide_to_utf8(L"\"" + self_path() + L"\" --app " + session);
    input(backend, request);
    request = {}; request.operation = Operation::KeyboardPress; request.key = Key::Enter;
    input(backend, request);
    Sleep(60000);
    return 3;
}
void test() {
    const auto session = L"SES-" + util::utf8_to_wide(scrp::uuid_v4()).value();
    Handle ready{CreateEventW(nullptr, TRUE, FALSE, (L"Local\\SCRP-Test-" + session).c_str())};
    check(ready.h != nullptr, "ready event");
    std::wstring command = L"\"" + self_path() + L"\" --creator " + session;
    STARTUPINFOW start{}; start.cb = sizeof(start); PROCESS_INFORMATION process{};
    check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &start, &process), "creator start");
    Handle parent{process.hProcess}, thread{process.hThread};
    try {
        check(WaitForSingleObject(ready.h, 15000) == WAIT_OBJECT_0, "child GUI did not start");
        HWND window = FindWindowW(L"ScrpOwnershipFixture", session.c_str());
        check(window != nullptr, "owned app window missing");
        DWORD child_pid = 0; GetWindowThreadProcessId(window, &child_pid);
        Handle child{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child_pid)};
        check(child.h != nullptr, "child handle");
        bool refused = false;
        try { auto duplicate = make_app_window_backend(util::wide_to_utf8(session)); }
        catch (const std::runtime_error&) { refused = true; }
        check(refused, "two controllers acquired one session");
        check(TerminateProcess(parent.h, 42) && WaitForSingleObject(parent.h, 5000) == WAIT_OBJECT_0,
              "creator crash not confirmed");
        check(WaitForSingleObject(child.h, 0) == WAIT_TIMEOUT, "crash killed owned app");
        HANDLE retained = OpenJobObjectW(JOB_OBJECT_QUERY, FALSE,
            (L"Local\\SCRP-App-" + session + L"-processes").c_str());
        std::cout << "job_after_crash=" << bool(retained) << "; win32=" << (retained ? 0 : GetLastError()) << '\n';
        if (retained) CloseHandle(retained);
        auto recovered = make_app_window_backend(util::wide_to_utf8(session));
        CaptureLimits limits;
        auto capture = recovered->capture(limits);
        check(capture.ok, "recovered app capture failed");
        int width = 0, height = 0; recovered->screen_size(width, height);
        RECT rect{}; GetWindowRect(window, &rect);
        check(width == rect.right - rect.left && height == rect.bottom - rect.top, "recovery selected launcher instead of app");
        HWND edit = GetDlgItem(window, 101); RECT edit_rect{};
        check(GetWindowRect(edit, &edit_rect), "fixture edit bounds");
        ActionRequest request; request.operation = Operation::MouseClick;
        request.x = edit_rect.right - rect.left - 10;
        request.y = (edit_rect.top + edit_rect.bottom) / 2 - rect.top;
        input(recovered, request);
        request = {}; request.operation = Operation::KeyboardType; request.text_utf8 = util::wide_to_utf8(L"복구");
        input(recovered, request);
        wchar_t value[128]{};
        SendMessageW(edit, WM_GETTEXT, 128, reinterpret_cast<LPARAM>(value));
        const std::wstring result(value);
        check(result.find(L"before-crash|") != std::wstring::npos && result.find(L"복구") != std::wstring::npos,
              "app state/input not preserved");
        auto other = make_app_window_backend(util::wide_to_utf8(session) + "-other");
        check(other->capture(limits).ok, "other controller capture");
        other->screen_size(width, height);
        check(width != rect.right - rect.left || height != rect.bottom - rect.top, "other session acquired old app");
        PostMessageW(window, WM_CLOSE, 0, 0);
        check(WaitForSingleObject(child.h, 5000) == WAIT_OBJECT_0, "fixture child cleanup");
    } catch (...) {
        if (WaitForSingleObject(parent.h, 0) == WAIT_TIMEOUT) TerminateProcess(parent.h, 43);
        if (HWND window = FindWindowW(L"ScrpOwnershipFixture", session.c_str())) PostMessageW(window, WM_CLOSE, 0, 0);
        throw;
    }
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 6 && wcscmp(argv[1], L"--app-job-keeper") == 0)
            return app_job_keeper(argv[2], argv[3], argv[4], argv[5]);
        if (argc == 3 && wcscmp(argv[1], L"--app") == 0) return app(argv[2]);
        if (argc == 3 && wcscmp(argv[1], L"--creator") == 0) return creator(argv[2]);
        check(argc == 1, "invalid arguments"); test();
        std::cout << "PASS: crashed controller's app survived; exact session Job reopened; Korean input and other-session isolation verified\n";
        return 0;
    } catch (const std::exception& error) { std::cout << "FAIL: " << error.what() << '\n'; return 1; }
}
