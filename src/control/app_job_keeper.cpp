#include "app_job_keeper.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace runner::control {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
bool valid(const std::wstring& value) {
    return !value.empty() && value.size() <= 80 && std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
               (c >= L'0' && c <= L'9') || c == L'-' || c == L'_';
    });
}
HANDLE handle(const wchar_t* text) {
    if (!text || !*text || wcsspn(text, L"0123456789") != wcslen(text)) return nullptr;
    wchar_t* end = nullptr;
    const auto parsed = wcstoull(text, &end, 10);
    if (*end || !parsed || parsed == UINTPTR_MAX) return nullptr;
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(parsed));
}
}
int app_job_keeper(const wchar_t* job_text, const wchar_t* ready_text, const wchar_t* owner_text, const wchar_t* session_text) {
    const std::wstring session = session_text ? session_text : L"";
    if (!valid(session)) return 2;
    Handle job{handle(job_text)}, ready{handle(ready_text)}, owner{handle(owner_text)};
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!job.value || !ready.value || !owner.value || !QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation,
            &accounting, sizeof(accounting), nullptr) || !SetEvent(ready.value)) return 3;
    for (;;) {
        if (!QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation,
                                      &accounting, sizeof(accounting), nullptr)) return 4;
        // Exact original Runner handle, never a reusable PID or its named
        // instance event (retaining that name would exclude a new controller).
        const auto state = WaitForSingleObject(owner.value, 0);
        if (state == WAIT_OBJECT_0 && !accounting.ActiveProcesses) return 0;
        if (state != WAIT_OBJECT_0 && state != WAIT_TIMEOUT) return 5;
        Sleep(250);
    }
}
void retain_app_job(HANDLE job, const std::wstring& session) {
    if (!valid(session)) throw std::runtime_error("invalid Job keeper identity");
    Handle inherited_job, ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)}, inherited_ready, inherited_owner;
    if (!ready.value || !DuplicateHandle(GetCurrentProcess(), job, GetCurrentProcess(), &inherited_job.value,
            JOB_OBJECT_QUERY, TRUE, 0) || !DuplicateHandle(GetCurrentProcess(), ready.value, GetCurrentProcess(),
            &inherited_ready.value, EVENT_MODIFY_STATE, TRUE, 0) ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &inherited_owner.value,
                         SYNCHRONIZE, TRUE, 0)) throw std::runtime_error("Job keeper handle preparation failed");
    wchar_t executable[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, executable, 32768);
    if (!count || count >= 32768) throw std::runtime_error("Job keeper executable unavailable");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --app-job-keeper " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(inherited_job.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(inherited_ready.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(inherited_owner.value)) + L" " + session;
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
        throw std::runtime_error("Job keeper attribute list unavailable");
    HANDLE handles[]{inherited_job.value, inherited_ready.value, inherited_owner.value};
    const bool prepared = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                    handles, sizeof(handles), nullptr, nullptr) != FALSE;
    PROCESS_INFORMATION process{};
    const bool started = prepared && CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!started) throw std::runtime_error("Job keeper did not start");
    Handle child{process.hProcess}, thread{process.hThread};
    HANDLE waiting[]{ready.value, child.value};
    if (WaitForMultipleObjects(2, waiting, FALSE, 5000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(child.value, 0) != WAIT_TIMEOUT) {
        // Only our just-created helper; no application/process enumeration.
        TerminateProcess(child.value, 6);
        WaitForSingleObject(child.value, 5000);
        throw std::runtime_error("Job keeper readiness not confirmed");
    }
}
}
