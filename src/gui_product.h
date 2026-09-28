#pragma once
#include "gui_session.h"
#include "transport/observation/https_uploader.h"

namespace runner {
// Trusted in-process integration boundary. No JSON fields or wire messages are
// defined here. There is deliberately no adapter for today's Host contract.
struct GuiProductServices {
    runtime::SandboxRuntime::Config runtime;
    observation::Destination observations;
    // Sends this exact probe through the agreed startup path and obtains Host
    // verification. Must honor stop/deadline; local capture is not Host approval.
    std::function<void(const control::CaptureResult&, HANDLE,
                       std::chrono::steady_clock::time_point)> prepare;
    // Fast lookup of a verified, connection-bound decision after HELLO_ACK.
    // Existing Runtime validates binding, policy, capabilities, checks and lease.
    GuiSession::Authorize authorize;
    // Fast lookup/consumption of an issued grant; no network on this callback.
    // The issuer must bind a separate upload credential, never a Control token.
    std::function<observation::UploadGrant(const scrp::Envelope&)> take_upload_grant;
};

// Product owner of probe, GUI, uploader and Control lifetime. Null means explicit
// management-only mode until an agreed Host adapter is linked by the launcher.
// No fake backend, permissive grant or test uploader is selected by the CLI.
DWORD run_product_control_session(::control::Context, HANDLE stop_event,
                                  const GuiProductServices* services = nullptr);
} // namespace runner
