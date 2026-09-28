#pragma once
#include "session_context.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace runner {
// Management-only Control mode: no GUI Worker, Artifact producer or Telemetry.
DWORD run_control_session(control::Context context, HANDLE stop_event);
control::ReplyHandlers management_handlers();
}
