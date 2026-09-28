#include "artifact/output_watcher.h"
#include "artifact/candidate_detector.h"
#include "runner_paths.h"
#include "runner_lifecycle.h"
#include "candidate_telemetry.h"
#include "control_session.h"
#include <iostream>
#include <cwchar>

namespace {
SRWLOCK stop_lock = SRWLOCK_INIT;
HANDLE stop_event = nullptr;

BOOL WINAPI on_console_event(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    AcquireSRWLockShared(&stop_lock);
    if (stop_event) SetEvent(stop_event);
    ReleaseSRWLockShared(&stop_lock);
    return TRUE;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return "";
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return "<invalid Unicode name>";
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), &result[0], size, nullptr, nullptr);
    // Escape control characters so a filename cannot forge additional log lines.
    std::string escaped;
    const char hex[] = "0123456789abcdef";
    for (unsigned char c : result) {
        if (c < 32 || c == 127) {
            escaped += "\\x";
            escaped += hex[c >> 4];
            escaped += hex[c & 15];
        } else if (c == '\\' || c == '"') {
            escaped += '\\';
            escaped += static_cast<char>(c);
        } else escaped += static_cast<char>(c);
    }
    return escaped;
}

void report(const artifact::OutputChange& change) {
    const char* kind = "UNKNOWN";
    switch (change.kind) {
    case artifact::ChangeKind::ready: kind = "WATCHING"; break;
    case artifact::ChangeKind::created: kind = "CREATED"; break;
    case artifact::ChangeKind::modified: kind = "MODIFIED"; break;
    case artifact::ChangeKind::removed: kind = "REMOVED"; break;
    case artifact::ChangeKind::renamed_old: kind = "RENAMED_OLD"; break;
    case artifact::ChangeKind::renamed_new: kind = "RENAMED_NEW"; break;
    }
    std::cout << kind << " \"" << utf8(change.name) << "\"" << std::endl;
}

void report_candidate(const artifact::CandidateStatus& status) {
    std::cout << (status.state == artifact::CandidateState::candidate ? "" : "ARTIFACT ")
              << artifact::candidate_state_name(status.state) << " \"" << utf8(status.path)
              << "\" generation=" << status.generation << " error=" << status.error
              << " " << status.detail << std::endl;
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wcscmp(argv[1], L"--help") == 0) {
        std::wcout << L"Usage: sandbox_runner.exe --session-context <local-context.json> [--output C:\\test-session\\Output]\n"
                   << L"   or: sandbox_runner.exe --control-context <control-context.json>\n"
                   << L"Default: " << runner::default_output_path
                   << L" (created if missing, watched recursively until stopped)\n"
                   << L"--output: use an existing directory for development/testing\n"
                   << L"--session-context: explicit local injection; see docs/telemetry.md\n";
        return 0;
    }
    std::wstring context_path;
    std::wstring control_path;
    std::wstring output_path = runner::default_output_path;
    bool supplied_output = false;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 < argc && std::wcscmp(argv[i], L"--session-context") == 0 && context_path.empty()) {
            context_path = argv[i + 1];
        } else if (i + 1 < argc && std::wcscmp(argv[i], L"--control-context") == 0 && control_path.empty()) {
            control_path = argv[i + 1];
        } else if (i + 1 < argc && std::wcscmp(argv[i], L"--output") == 0 && !supplied_output) {
            output_path = argv[i + 1];
            supplied_output = true;
        } else {
            std::cerr << "Invalid Runner arguments; use --help.\n";
            return 2;
        }
    }
    if ((context_path.empty() && control_path.empty()) ||
        (!control_path.empty() && (!context_path.empty() || supplied_output))) {
        std::cerr << "Choose --session-context or --control-context; Control mode has no Output watcher.\n";
        return 2;
    }
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event) {
        std::cerr << "Cannot create stop event: " << GetLastError() << '\n';
        return 1;
    }
    // A launcher may have disabled Ctrl+C; that setting is inherited by children.
    if (!SetConsoleCtrlHandler(nullptr, FALSE) || !SetConsoleCtrlHandler(on_console_event, TRUE)) {
        const DWORD error = GetLastError();
        CloseHandle(stop_event);
        stop_event = nullptr;
        std::cerr << "Cannot register console handler: " << error << '\n';
        return 1;
    }
    DWORD result = ERROR_GEN_FAILURE;
    try {
        if (!control_path.empty()) {
            result = runner::run_control_session(runner::load_control_context(control_path), stop_event);
        } else {
            auto context = runner::load_session_context(context_path);
            // A Host adapter may implement both contracts. The current local
            // handshake-only input remains supported; event_contract selects the
            // artifact adapter explicitly. No implicit wire contract fallback.
            auto candidate_contract = std::dynamic_pointer_cast<const runner::CandidateEventContract>(context.schema);
            auto telemetry = runner::start_telemetry(std::move(context), stop_event);
            std::cout << "Telemetry READY\n" << std::flush;
            if (!supplied_output) {
                const DWORD error = artifact::prepare_output_directory(output_path);
                if (error != ERROR_SUCCESS)
                    throw std::runtime_error("Cannot prepare session Output (Win32 error " + std::to_string(error) + ")");
            }
            std::cout << "Runner started\nOutput: \"" << utf8(output_path) << "\" (recursive)" << std::endl;
            // Creation order deliberately makes exception unwinding stop the
            // candidate producer before destroying/joining the Telemetry client.
            runner::CandidateTelemetry candidate_telemetry(*telemetry, std::move(candidate_contract),
                [](const char* diagnostic) { std::cerr << diagnostic << '\n'; });
            artifact::CandidateDetector candidates(output_path,
                [&candidate_telemetry](const artifact::CandidateStatus& status) {
                    candidate_telemetry.submit(status);
                    report_candidate(status);
                }, report);
            try {
                result = artifact::watch_output(output_path, stop_event,
                    [&candidates](const artifact::OutputChange& change) { candidates.submit(change); });
            } catch (...) {
                candidates.invalidate(ERROR_GEN_FAILURE);
                throw;
            }
            if (result != ERROR_SUCCESS) candidates.invalidate(result);
            candidates.stop();
            // First drain/cancel watcher I/O and stop/join candidate producers. Then
            // reject new Telemetry work, cancel WSS I/O and join its worker. Pending
            // events remain inspectable until client destruction; they are not ACKed.
            telemetry->stop();
            const auto final_telemetry = telemetry->snapshot();
            std::cout << "Telemetry final pending=" << final_telemetry.pending_events
                      << " expired=" << final_telemetry.expired_events
                      << " rejected=" << final_telemetry.rejected_events
                      << " errors=" << final_telemetry.errors << '\n';
            if (final_telemetry.pending_events)
                std::cerr << "Telemetry stopped with unacknowledged events.\n";
            std::cout << "Telemetry stopped\n";
        }
    } catch (const std::exception& error) {
        result = ERROR_GEN_FAILURE;
        std::cerr << "Runner initialization/lifecycle failed: " << error.what() << '\n';
    } catch (...) {
        result = ERROR_GEN_FAILURE;
        std::cerr << "Runner initialization/lifecycle failed.\n";
    }
    AcquireSRWLockExclusive(&stop_lock);
    CloseHandle(stop_event);
    stop_event = nullptr;
    ReleaseSRWLockExclusive(&stop_lock);
    SetConsoleCtrlHandler(on_console_event, FALSE);
    if (result != ERROR_SUCCESS) {
        std::cerr << "Runner stopped with failure (Win32 result " << result << ").\n";
        return 1;
    }
    std::cout << (control_path.empty() ? "Watch stopped\n" : "Runner stopped\n");
    return 0;
}
