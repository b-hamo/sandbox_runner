#pragma once
#include "session_context.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace runner {
// Returns only after READY; startup failure/cancellation stops and joins the
// client before throwing. The caller owns stop_event for the whole call.
std::unique_ptr<telemetry::TelemetryClient> start_telemetry(
    SessionContext context, HANDLE stop_event,
    std::chrono::milliseconds ready_timeout = std::chrono::seconds(30),
    telemetry::ClientOptions options = {},
    telemetry::TelemetryClient::SocketFactory factory = telemetry::make_winhttp_websocket);
} // namespace runner
