#include "app_window_backend.h"
#include "control_util.h"
#include <windows.h>
#include <stdexcept>
#include <vector>

namespace runner::control {
namespace {
constexpr UINT show_launch = WM_APP + 21;
class AppWindowBackend final : public WorkerBackend {
    HWND panel_ = nullptr, edit_ = nullptr, button_ = nullptr, target_ = nullptr, focus_ = nullptr;
    HANDLE ready_ = nullptr, job_ = nullptr, target_process_ = nullptr;
    DWORD target_pid_ = 0;
    std::thread ui_;
    std::wstring title_;
    unsigned launch_serial_ = 0;
    bool launch_visible_ = false;
    std::function<ErrorCode()> gate_;
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<AppWindowBackend*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<AppWindowBackend*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        if (message == show_launch) {
            SetWindowTextW(self->edit_, L"");
            ShowWindow(self->edit_, SW_SHOW);
            ShowWindow(self->button_, SW_SHOW);
            ShowWindow(window, SW_SHOWNOACTIVATE);
            SetFocus(self->edit_);
            return 1;
        }
        if (message == WM_COMMAND && LOWORD(wp) == 101) {
            wchar_t text[4097]{};
            GetWindowTextW(self->edit_, text, 4097);
            // Keep actual child startup errors in this VM, rather than treating
            // process creation as an application success. Only these three
            // handles are inherited; never inherit Runner's TLS/file handles.
            CreateDirectoryW(L"C:\\RunnerLogs", nullptr);
            const auto log_path = L"C:\\RunnerLogs\\app-launch-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(++self->launch_serial_) + L".txt";
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       &security, OPEN_EXISTING, 0, nullptr);
            STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.lpDesktop = const_cast<wchar_t*>(L"winsta0\\default");
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = input;
            startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log;
            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            std::vector<unsigned char> attributes(bytes);
            startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            HANDLE inherited[]{log, input};
            const bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes) != FALSE;
            const bool prepared = initialized && log != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE &&
                UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                          inherited, sizeof(inherited), nullptr, nullptr);
            PROCESS_INFORMATION process{};
            const bool started = *text && prepared && CreateProcessW(nullptr, text, nullptr, nullptr, TRUE,
                CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                nullptr, nullptr, &startup.StartupInfo, &process);
            if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
            if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            if (!started) {
                SetWindowTextW(window, L"SCRP app launch failed; command was not started");
                return 0;
            }
            if (!AssignProcessToJobObject(self->job_, process.hProcess)) {
                TerminateProcess(process.hProcess, ERROR_ACCESS_DENIED);
                CloseHandle(process.hThread); CloseHandle(process.hProcess);
                SetWindowTextW(window, L"SCRP app launch failed: process ownership");
                return 0;
            }
            const DWORD resumed = ResumeThread(process.hThread);
            if (resumed == static_cast<DWORD>(-1)) TerminateProcess(process.hProcess, ERROR_GEN_FAILURE);
            CloseHandle(process.hThread); CloseHandle(process.hProcess);
            SetWindowTextW(window, resumed == static_cast<DWORD>(-1) ? L"SCRP app launch failed" : self->title_.c_str());
            if (resumed != static_cast<DWORD>(-1)) {
                ShowWindow(self->edit_, SW_HIDE); ShowWindow(self->button_, SW_HIDE);
            }
            return resumed == static_cast<DWORD>(-1) ? 0 : 1;
        }
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wp, lp);
    }
    void ui_loop() {
        WNDCLASSW type{};
        type.lpfnWndProc = window_proc; type.hInstance = GetModuleHandleW(nullptr);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        type.lpszClassName = L"ScrpAppLaunch";
        RegisterClassW(&type);
        panel_ = CreateWindowExW(0, type.lpszClassName, title_.c_str(),
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 620, 240, nullptr, nullptr, type.hInstance, this);
        if (panel_) {
            CreateWindowW(L"STATIC", L"App window mode. Win+R opens this command field; Enter starts the command.",
                WS_CHILD | WS_VISIBLE, 20, 20, 570, 40, panel_, nullptr, type.hInstance, nullptr);
            edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                20, 75, 450, 36, panel_, reinterpret_cast<HMENU>(100), type.hInstance, nullptr);
            button_ = CreateWindowW(L"BUTTON", L"Run", WS_CHILD | BS_PUSHBUTTON,
                480, 75, 100, 36, panel_, reinterpret_cast<HMENU>(101), type.hInstance, nullptr);
            SetFocus(edit_);
        }
        SetEvent(ready_);
        if (!panel_) return;
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    bool valid() const {
        DWORD pid = 0;
        return target_ && IsWindow(target_) && GetWindowThreadProcessId(target_, &pid) &&
            pid == target_pid_ && target_process_ && WaitForSingleObject(target_process_, 0) == WAIT_TIMEOUT;
    }
    void bind(HWND window) {
        if (window == target_ && valid()) return;
        if (target_process_) CloseHandle(target_process_);
        target_ = window; focus_ = nullptr; target_pid_ = 0;
        GetWindowThreadProcessId(window, &target_pid_);
        target_process_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target_pid_);
        if (window == panel_) focus_ = edit_;
        else {
            GUITHREADINFO info{}; info.cbSize = sizeof(info);
            if (GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info) &&
                (info.hwndFocus == window || IsChild(window, info.hwndFocus))) focus_ = info.hwndFocus;
            if (!focus_) focus_ = window;
        }
    }
    static BOOL CALLBACK select_window(HWND window, LPARAM context) {
        auto* self = reinterpret_cast<AppWindowBackend*>(context);
        if (!IsWindowVisible(window) || IsIconic(window) || GetWindow(window, GW_OWNER) || window == self->panel_) return TRUE;
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        BOOL owned = FALSE;
        if (process) { IsProcessInJob(process, self->job_, &owned); CloseHandle(process); }
        RECT bounds{};
        if (!owned || !GetWindowRect(window, &bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return TRUE;
        self->bind(window); return FALSE;
    }
    static bool message(HWND window, UINT msg, WPARAM wp, LPARAM lp, DWORD_PTR* answer = nullptr) {
        DWORD_PTR result = 0;
        const bool ok = SendMessageTimeoutW(window, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) != 0;
        if (answer) *answer = result;
        return ok;
    }
public:
    void set_execution_gate(std::function<ErrorCode()> gate) override { gate_ = std::move(gate); }
    explicit AppWindowBackend(const std::string& session) {
        if (!enable_per_monitor_dpi_awareness()) throw std::runtime_error("app surface DPI awareness unavailable");
        title_ = L"SCRP app window - " + util::utf8_to_wide(session).value();
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!ready_ || !job_) throw std::runtime_error("app surface handles unavailable");
        ui_ = std::thread([this] { ui_loop(); });
        WaitForSingleObject(ready_, INFINITE);
        if (!panel_ || !edit_) { ui_.join(); throw std::runtime_error("app surface window unavailable"); }
        bind(panel_);
    }
    ~AppWindowBackend() override {
        if (panel_) PostMessageW(panel_, WM_CLOSE, 0, 0);
        if (ui_.joinable()) ui_.join();
        if (target_process_) CloseHandle(target_process_);
        if (ready_) CloseHandle(ready_);
        if (job_) CloseHandle(job_); // No kill-on-close: Runner disconnect must not terminate app work.
    }
    CaptureResult capture(const CaptureLimits& limits) override {
        if (!launch_visible_) EnumWindows(select_window, reinterpret_cast<LPARAM>(this));
        // A completed application leaves no window to observe. Return the real
        // launcher panel so a subsequent Router request can obtain a fresh
        // observation before Win+R. Never reuse the closed application's frame.
        if (!valid() && IsWindow(panel_)) bind(panel_);
        if (!valid()) { CaptureResult result; result.error = "app surface unavailable"; return result; }
        auto result = capture_window_png(target_, limits);
        if (!valid()) { result.ok = false; result.png.clear(); result.error = "app target exited during capture"; }
        return result;
    }
    void screen_size(int& width, int& height) override {
        RECT rect{};
        width = height = 0;
        if (valid() && GetWindowRect(target_, &rect)) { width = rect.right - rect.left; height = rect.bottom - rect.top; }
    }
    ExecOutcome input(const ActionRequest& request, std::chrono::steady_clock::time_point deadline,
                      const std::atomic<bool>& cancelled) override {
        ExecOutcome result{ActionStatus::Failed, ErrorCode::InputRejected, "app-window input not delivered", 0};
        auto alive = [&] { return !cancelled && std::chrono::steady_clock::now() < deadline &&
                                (!gate_ || gate_() == ErrorCode::None) && valid(); };
        if (request.operation == Operation::KeyboardHotkey && request.key == Key::R &&
            request.modifiers == std::vector<Modifier>{Modifier::Win}) {
            if (cancelled || std::chrono::steady_clock::now() >= deadline) return result;
            if (!message(panel_, show_launch, 0, 0)) return result;
            launch_visible_ = true; bind(panel_);
        } else {
            if (!alive()) return result;
            switch (request.operation) {
            case Operation::KeyboardType: {
                if (target_ == panel_ && !launch_visible_) return result;
                auto text = util::utf8_to_wide(request.text_utf8);
                if (!text) return result;
                for (wchar_t character : *text) {
                    if (!alive() || !message(focus_, WM_CHAR, character, 1)) {
                        result.status = result.units_sent ? ActionStatus::PartialSuccess : ActionStatus::Failed;
                        result.message = "app text delivery interrupted; do not resend";
                        return result;
                    }
                    ++result.units_sent;
                }
                break;
            }
            case Operation::KeyboardPress: {
                if (request.key == Key::Enter && target_ == panel_) {
                    DWORD_PTR started = 0;
                    if (!message(panel_, WM_COMMAND, 101, 0, &started) || !started) return result;
                    launch_visible_ = false;
                } else {
                    UINT key = request.key == Key::Enter ? VK_RETURN : request.key == Key::Tab ? VK_TAB :
                        request.key == Key::Escape ? VK_ESCAPE : request.key == Key::Backspace ? VK_BACK :
                        request.key == Key::Delete ? VK_DELETE : 0;
                    if (!key || !message(focus_, WM_KEYDOWN, key, 1)) return result;
                    if ((key == VK_RETURN || key == VK_BACK || key == VK_TAB) && !message(focus_, WM_CHAR, key, 1)) return result;
                    if (!message(focus_, WM_KEYUP, key, 0xc0000001)) return result;
                }
                break;
            }
            case Operation::KeyboardHotkey: {
                wchar_t name[128]{}; GetClassNameW(focus_, name, 128);
                if (request.key != Key::A || request.modifiers != std::vector<Modifier>{Modifier::Ctrl} ||
                    (wcsstr(name, L"EDIT") == nullptr && wcsstr(name, L"Edit") == nullptr)) return result;
                if (!message(focus_, EM_SETSEL, 0, -1)) return result;
                break;
            }
            case Operation::MouseClick:
            case Operation::MouseMove: {
                if (!request.x || !request.y || request.button != MouseButton::Left) return result;
                RECT rect{}; if (!GetWindowRect(target_, &rect)) return result;
                POINT point{rect.left + *request.x, rect.top + *request.y};
                HWND child = target_;
                for (int depth = 0; depth < 16; ++depth) {
                    POINT local = point; ScreenToClient(child, &local);
                    HWND next = ChildWindowFromPointEx(child, local, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
                    if (!next || next == child) break;
                    child = next;
                }
                ScreenToClient(child, &point);
                LPARAM coordinates = MAKELPARAM(point.x, point.y);
                if (!message(child, WM_MOUSEMOVE, 0, coordinates)) return result;
                if (request.operation == Operation::MouseClick) {
                    focus_ = child;
                    for (int count = 0; count < request.click_count; ++count)
                        if (!alive() || !message(child, WM_LBUTTONDOWN, MK_LBUTTON, coordinates) ||
                            !message(child, WM_LBUTTONUP, 0, coordinates)) return result;
                    if (target_ == panel_ && GetDlgCtrlID(child) == 101) launch_visible_ = false;
                }
                break;
            }
            default: result.message = "operation unsupported by app-window surface"; return result;
            }
        }
        result.status = ActionStatus::Success; result.error = ErrorCode::None;
        result.message = "app-window input delivered; application outcome requires observation";
        return result;
    }
};
}
std::shared_ptr<WorkerBackend> make_app_window_backend(const std::string& session) {
    return std::make_shared<AppWindowBackend>(session);
}
bool is_app_window_backend(const std::shared_ptr<WorkerBackend>& backend) {
    return dynamic_cast<AppWindowBackend*>(backend.get()) != nullptr;
}
}
