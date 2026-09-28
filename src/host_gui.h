#pragma once
#include "control_session.h"
namespace runner {
// Explicit host_control 6055cc6 profile. Host Broker owns startup/input policy.
// Bootstrap is trusted launcher input; TLS chain + hostname validation is retained.
DWORD run_host_gui(const std::wstring& bootstrap, const std::wstring& host_address, HANDLE stop);
}
