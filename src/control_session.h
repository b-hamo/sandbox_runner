#pragma once
#include "session_context.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace runner {
class GuiSession;
// GUI requires trusted Host adapters; nullptr preserves management-only mode.
DWORD run_control_session(::control::Context context, HANDLE stop_event, GuiSession* gui = nullptr);
::control::ReplyHandlers management_handlers();
}
