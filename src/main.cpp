#include "artifact/output_watcher.h"
#include "runner_paths.h"
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
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc == 2 && std::wcscmp(argv[1], L"--help") == 0) {
        std::wcout << L"Usage: sandbox_runner.exe [--output C:\\test-session\\Output]\n"
                   << L"Default: " << runner::default_output_path
                   << L" (created if missing, watched recursively until stopped)\n"
                   << L"--output: use an existing directory for development/testing\n";
        return 0;
    }
    if (argc != 1 && (argc != 3 || std::wcscmp(argv[1], L"--output") != 0)) {
        std::cerr << "Usage: sandbox_runner.exe [--output C:\\test-session\\Output]\n";
        return 2;
    }
    std::cout << "Runner started\n";
    const std::wstring output_path = argc == 1 ? runner::default_output_path : argv[2];
    if (argc == 1) {
        const DWORD error = artifact::prepare_output_directory(output_path);
        if (error != ERROR_SUCCESS) {
            std::cerr << "Cannot prepare session Output (Win32 error " << error << ").\n";
            return 1;
        }
    }
    std::cout << "Output: \"" << utf8(output_path) << "\" (recursive)" << std::endl;
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
        result = artifact::watch_output(output_path, stop_event, report);
    } catch (const std::exception& error) {
        std::cerr << "Watch exception: " << error.what() << '\n';
    }
    AcquireSRWLockExclusive(&stop_lock);
    CloseHandle(stop_event);
    stop_event = nullptr;
    ReleaseSRWLockExclusive(&stop_lock);
    SetConsoleCtrlHandler(on_console_event, FALSE);
    if (result != ERROR_SUCCESS) {
        std::cerr << "Output watch failed (Win32 error " << result
                  << "). Watch stopped; events may be missing.\n";
        return 1;
    }
    std::cout << "Watch stopped\n";
    return 0;
}
