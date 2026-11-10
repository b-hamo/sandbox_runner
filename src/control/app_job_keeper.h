#pragma once
#include <windows.h>
#include <string>
namespace runner::control {
// Guest-only helper. Receives QUERY Job, ready event and SYNCHRONIZE Runner
// handles; no socket, token, file or app handles. Never kills an app or runs code.
void retain_app_job(HANDLE job, const std::wstring& session);
int app_job_keeper(const wchar_t* job, const wchar_t* ready, const wchar_t* owner, const wchar_t* session);
}
