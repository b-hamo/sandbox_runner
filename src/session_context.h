#pragma once
#include "transport/telemetry/telemetry_client.h"

namespace runner {
// Reuse the existing immutable session + transport contract, including the
// externally supplied credential provider and TelemetrySchema. No second copy
// of session identity, Runtime generation or TLS settings is maintained.
using SessionContext = telemetry::Context;

void validate_session_context(const SessionContext& context);

// Explicit local/bootstrap injection, not a Control HELLO_ACK wire format.
// Contains credentials: the launcher owns file access permissions and removal.
SessionContext load_session_context(const std::wstring& path);
} // namespace runner
