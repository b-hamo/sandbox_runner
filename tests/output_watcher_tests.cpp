// Host-only integration tests. This entry point belongs to output_watcher_tests,
// not sandbox_runner; do not include this source in the deployed Runner target.
#include "artifact/output_watcher.h"
#include "runner_paths.h"
#include <winioctl.h>
#include <cstring>

#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Watch {
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::mutex mutex;
    std::vector<artifact::OutputChange> changes;
    DWORD result = ERROR_GEN_FAILURE;
    std::thread thread;

    explicit Watch(const std::wstring& path) {
        require(stop && ready, "CreateEvent failed");
        thread = std::thread([this, path] {
            result = artifact::watch_output(path, stop, [this](const artifact::OutputChange& c) {
                if (c.kind == artifact::ChangeKind::ready) SetEvent(ready);
                else {
                    std::lock_guard<std::mutex> lock(mutex);
                    changes.push_back(c);
                }
            });
        });
    }
    ~Watch() {
        SetEvent(stop);
        if (thread.joinable()) thread.join();
        CloseHandle(ready);
        CloseHandle(stop);
    }
    void wait_ready() { require(WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0, "watch not ready"); }
    bool contains(artifact::ChangeKind kind, const std::wstring& name) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& c : changes) if (c.kind == kind && c.name == name) return true;
        return false;
    }
    void expect(artifact::ChangeKind kind, const std::wstring& name) {
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < 5000) {
            if (contains(kind, name)) return;
            Sleep(10);
        }
        throw std::runtime_error("expected file event missing");
    }
    void finish() {
        SetEvent(stop);
        thread.join();
        require(result == ERROR_SUCCESS, "watch failed instead of stopping");
    }
};

void write_file(const std::wstring& path, bool append = false) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, append ? OPEN_EXISTING : CREATE_NEW, 0, nullptr);
    require(file != INVALID_HANDLE_VALUE, "cannot open test file");
    if (append) SetFilePointer(file, 0, nullptr, FILE_END);
    DWORD bytes = 0;
    const BOOL written = WriteFile(file, "test", 4, &bytes, nullptr);
    CloseHandle(file);
    require(written && bytes == 4, "cannot write test file");
}

// Junctions can be created without the symbolic-link privilege on local NTFS.
void make_junction(const std::wstring& link, const std::wstring& target) {
    require(CreateDirectoryW(link.c_str(), nullptr) != 0, "cannot create junction directory");
    const std::wstring substitute = L"\\??\\" + target;
    struct JunctionData {
        DWORD tag;
        WORD data_length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
        wchar_t path[1];
    };
    const std::size_t path_bytes = (substitute.size() + 1 + target.size() + 1) * sizeof(wchar_t);
    const DWORD total = static_cast<DWORD>(16 + path_bytes);
    std::vector<DWORD> storage((total + 3) / 4, 0);
    auto* data = reinterpret_cast<JunctionData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->data_length = static_cast<WORD>(total - 8);
    data->substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    data->print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    std::memcpy(data->path, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<unsigned char*>(data->path) + data->print_offset,
                target.c_str(), (target.size() + 1) * sizeof(wchar_t));
    HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(directory != INVALID_HANDLE_VALUE, "cannot open junction directory");
    DWORD bytes = 0;
    const BOOL result = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, data, total,
                                        nullptr, 0, &bytes, nullptr);
    CloseHandle(directory);
    require(result != 0, "cannot set junction target");
}

struct Workspace {
    std::wstring root;
    std::wstring output;
    Workspace() {
        wchar_t temp[MAX_PATH] = {};
        wchar_t unique[MAX_PATH] = {};
        require(GetTempPathW(MAX_PATH, temp) != 0, "GetTempPath failed");
        require(GetTempFileNameW(temp, L"srt", 0, unique) != 0, "GetTempFileName failed");
        require(DeleteFileW(unique) != 0, "temp cleanup failed");
        root = unique;
        output = root + L"\\Output-\uc138\uc158";
        require(CreateDirectoryW(root.c_str(), nullptr) != 0, "cannot create workspace");
        require(CreateDirectoryW(output.c_str(), nullptr) != 0, "cannot create Output");
    }
    ~Workspace() {
        // Only remove known test paths; never recursively delete a supplied path.
        DeleteFileW((output + L"\\test-\ud55c\uae00.txt").c_str());
        DeleteFileW((output + L"\\renamed.txt").c_str());
        DeleteFileW((output + L"\\nested\\child.txt").c_str());
        DeleteFileW((output + L"\\nested\\renamed-child.txt").c_str());
        DeleteFileW((output + L"\\new-dir\\deep\\child.txt").c_str());
        DeleteFileW((output + L"\\moved-dir\\deep\\child.txt").c_str());
        RemoveDirectoryW((output + L"\\new-dir\\deep").c_str());
        RemoveDirectoryW((output + L"\\moved-dir\\deep").c_str());
        RemoveDirectoryW((output + L"\\new-dir").c_str());
        RemoveDirectoryW((output + L"\\moved-dir").c_str());
        RemoveDirectoryW((output + L"\\existing-link").c_str());
        RemoveDirectoryW((output + L"\\new-link").c_str());
        DeleteFileW((root + L"\\external\\outside-link.txt").c_str());
        RemoveDirectoryW((root + L"\\external").c_str());
        DeleteFileW((root + L"\\prepared\\Output\\preserved.txt").c_str());
        RemoveDirectoryW((root + L"\\prepared\\Output").c_str());
        RemoveDirectoryW((root + L"\\prepared").c_str());
        DeleteFileW((root + L"\\outside.txt").c_str());
        RemoveDirectoryW((output + L"\\nested").c_str());
        RemoveDirectoryW((root + L"\\alias").c_str());
        RemoveDirectoryW(output.c_str());
        RemoveDirectoryW(root.c_str());
    }
};
// Exercise the actual Runner's console handler in a hidden Windows console.
void check_runner_shutdown(const std::wstring& exe, const std::wstring& output) {
    SECURITY_ATTRIBUTES security = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    require(CreatePipe(&reader, &writer, &security, 0) != 0, "CreatePipe failed");
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &security, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input;
    startup.hStdOutput = writer;
    startup.hStdError = writer;
    PROCESS_INFORMATION process = {};
    std::wstring command = L"\"" + exe + L"\" --output \"" + output + L"\"";
    const BOOL launched = CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, TRUE,
                                         CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process);
    CloseHandle(writer);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (!launched) { CloseHandle(reader); throw std::runtime_error("Runner launch failed"); }
    bool attached = false;
    try {
        std::string log;
        const ULONGLONG start = GetTickCount64();
        while (log.find("WATCHING") == std::string::npos && GetTickCount64() - start < 5000) {
            DWORD available = 0;
            require(PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) != 0,
                    "cannot read Runner output");
            if (available) {
                char buffer[1024];
                DWORD read = 0;
                require(ReadFile(reader, buffer, (available < sizeof(buffer) ? available : sizeof(buffer)),
                                 &read, nullptr) != 0, "Runner output read failed");
                log.append(buffer, read);
            } else Sleep(10);
        }
        require(log.find("WATCHING") != std::string::npos, "Runner did not start watching");
        const std::wstring candidate_path = output + L"\\runner-candidate.txt";
        write_file(candidate_path);
        const ULONGLONG candidate_start = GetTickCount64();
        const std::string expected = "ARTIFACT_CANDIDATE \"runner-candidate.txt\"";
        while (log.find(expected) == std::string::npos && GetTickCount64() - candidate_start < 5000) {
            DWORD available = 0;
            require(PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) != 0,
                    "cannot read candidate output");
            if (available) {
                char buffer[4096]; DWORD count = 0;
                require(ReadFile(reader, buffer, (available < sizeof(buffer) ? available : sizeof(buffer)),
                                 &count, nullptr) != 0, "candidate output read failed");
                log.append(buffer, count);
            } else Sleep(10);
        }
        DeleteFileW(candidate_path.c_str());
        require(log.find(expected) != std::string::npos, "Runner did not report stable candidate");
        require(log.find("PRESCAN") == std::string::npos && log.find("NO_DETECTION") == std::string::npos,
                "Runner still reports malware scan results");
        std::cout << "PASS Runner EXE ARTIFACT_CANDIDATE output\n";
        FreeConsole();
        require(AttachConsole(process.dwProcessId) != 0, "cannot attach to hidden Runner console");
        attached = true;
        require(SetConsoleCtrlHandler(nullptr, TRUE) != 0, "cannot protect test console handler");
        require(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != 0, "cannot send Ctrl+C");
        require(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "Runner did not exit");
        DWORD exit_code = 1;
        require(GetExitCodeProcess(process.hProcess, &exit_code) != 0 && exit_code == 0,
                "Runner Ctrl+C exit was unsuccessful");
        FreeConsole();
        attached = false;
        std::cout << "PASS Runner EXE Ctrl+C shutdown\n";
    } catch (...) {
        if (attached) FreeConsole();
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(reader);
        throw;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(reader);
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    try {
        Workspace workspace;
        require(CreateDirectoryW((workspace.output + L"\\nested").c_str(), nullptr) != 0,
                "cannot create nested directory");
        require(CreateDirectoryW((workspace.root + L"\\external").c_str(), nullptr) != 0,
                "cannot create external directory");
        make_junction(workspace.output + L"\\existing-link", workspace.root + L"\\external");
        {
            Watch watch(workspace.output);
            watch.wait_ready();
            const std::wstring name = L"test-\ud55c\uae00.txt";
            write_file(workspace.output + L"\\" + name);
            watch.expect(artifact::ChangeKind::created, name);
            // Drain any creation-related writes before testing a new modification.
            Sleep(100);
            {
                std::lock_guard<std::mutex> lock(watch.mutex);
                watch.changes.clear();
            }
            write_file(workspace.output + L"\\" + name, true);
            watch.expect(artifact::ChangeKind::modified, name);
            require(MoveFileW((workspace.output + L"\\" + name).c_str(),
                              (workspace.output + L"\\renamed.txt").c_str()) != 0, "rename failed");
            watch.expect(artifact::ChangeKind::renamed_old, name);
            watch.expect(artifact::ChangeKind::renamed_new, L"renamed.txt");
            write_file(workspace.output + L"\\nested\\child.txt");
            watch.expect(artifact::ChangeKind::created, L"nested\\child.txt");
            Sleep(100);
            {
                std::lock_guard<std::mutex> lock(watch.mutex);
                watch.changes.clear();
            }
            write_file(workspace.output + L"\\nested\\child.txt", true);
            watch.expect(artifact::ChangeKind::modified, L"nested\\child.txt");
            require(MoveFileW((workspace.output + L"\\nested\\child.txt").c_str(),
                              (workspace.output + L"\\nested\\renamed-child.txt").c_str()) != 0,
                    "nested file rename failed");
            watch.expect(artifact::ChangeKind::renamed_new, L"nested\\renamed-child.txt");
            require(DeleteFileW((workspace.output + L"\\nested\\renamed-child.txt").c_str()) != 0,
                    "nested file deletion failed");
            watch.expect(artifact::ChangeKind::removed, L"nested\\renamed-child.txt");
            require(CreateDirectoryW((workspace.output + L"\\new-dir").c_str(), nullptr) != 0,
                    "new directory failed");
            require(CreateDirectoryW((workspace.output + L"\\new-dir\\deep").c_str(), nullptr) != 0,
                    "deep directory failed");
            write_file(workspace.output + L"\\new-dir\\deep\\child.txt");
            watch.expect(artifact::ChangeKind::created, L"new-dir\\deep\\child.txt");
            require(MoveFileW((workspace.output + L"\\new-dir").c_str(),
                              (workspace.output + L"\\moved-dir").c_str()) != 0, "directory rename failed");
            watch.expect(artifact::ChangeKind::renamed_new, L"moved-dir");
            write_file(workspace.output + L"\\moved-dir\\deep\\child.txt", true);
            watch.expect(artifact::ChangeKind::modified, L"moved-dir\\deep\\child.txt");
            make_junction(workspace.output + L"\\new-link", workspace.root + L"\\external");
            write_file(workspace.root + L"\\outside.txt");
            write_file(workspace.root + L"\\external\\outside-link.txt");
            Sleep(300);
            {
                std::lock_guard<std::mutex> lock(watch.mutex);
                for (const auto& c : watch.changes) {
                    require(c.name != L"outside.txt" && c.name.find(L"outside-link.txt") == std::wstring::npos,
                            "out-of-scope file reported");
                }
            }
            require(DeleteFileW((workspace.output + L"\\renamed.txt").c_str()) != 0, "delete failed");
            watch.expect(artifact::ChangeKind::removed, L"renamed.txt");
            const ULONGLONG start = GetTickCount64();
            watch.finish();
            require(GetTickCount64() - start < 2000, "stop took too long");
        }
        std::cout << "PASS create/modify/rename/delete, Unicode, scope and active stop\n";
        std::cout << "PASS recursive changes, new/renamed directories and junction target exclusion\n";

        const std::wstring prepared = workspace.root + L"\\prepared\\Output";
        require(artifact::prepare_output_directory(prepared) == ERROR_SUCCESS, "Output preparation failed");
        write_file(prepared + L"\\preserved.txt");
        require(artifact::prepare_output_directory(prepared) == ERROR_SUCCESS, "repeated preparation failed");
        require(GetFileAttributesW((prepared + L"\\preserved.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
                "existing output removed");
        require(artifact::prepare_output_directory(workspace.output + L"\\existing-link\\escape") == ERROR_ACCESS_DENIED,
                "preparation followed a junction");
        require(GetFileAttributesW((workspace.root + L"\\external\\escape").c_str()) == INVALID_FILE_ATTRIBUTES,
                "preparation created directory outside Output");
        std::cout << "PASS directory preparation, preservation and junction rejection\n";

        DWORD before = 0, after = 0;
        require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "handle count failed");
        for (int i = 0; i < 20; ++i) {
            Watch watch(workspace.output);
            watch.wait_ready();
            watch.finish();
        }
        require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "handle count failed");
        require(after <= before, "handles leaked after repeated idle stop");
        std::cout << "PASS repeated start/idle stop and handle cleanup\n";

        HANDLE stop = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        require(stop != nullptr, "stop event failed");
        const auto ignore = [](const artifact::OutputChange&) {};
        require(artifact::watch_output(L"relative", stop, ignore) == ERROR_BAD_PATHNAME,
                "relative path accepted");
        require(artifact::watch_output(workspace.output + L"\\..\\Output", stop, ignore) == ERROR_BAD_PATHNAME,
                "path traversal accepted");
        require(artifact::watch_output(workspace.root + L"\\missing", stop, ignore) != ERROR_SUCCESS,
                "missing directory accepted");
        require(artifact::watch_output(workspace.root + L"\\outside.txt", stop, ignore) != ERROR_SUCCESS,
                "regular file accepted as directory");
        require(artifact::watch_output(workspace.output, stop, ignore) == ERROR_SUCCESS,
                "already requested stop failed");
        const std::wstring alias = workspace.root + L"\\alias";
        if (CreateSymbolicLinkW(alias.c_str(), workspace.output.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 2)) {
            require(artifact::watch_output(alias, stop, ignore) == ERROR_ACCESS_DENIED,
                    "reparse Output accepted");
            require(artifact::watch_output(alias + L"\\nested", stop, ignore) == ERROR_ACCESS_DENIED,
                    "reparse ancestor accepted");
            std::cout << "PASS reparse Output and ancestor rejection\n";
        } else {
            std::cout << "SKIP symlink test: Win32 error " << GetLastError() << '\n';
        }
        CloseHandle(stop);
        std::cout << "PASS invalid paths and pre-signalled stop\n";
        require(argc == 2, "Runner EXE path required");
        check_runner_shutdown(argv[1], workspace.output);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
