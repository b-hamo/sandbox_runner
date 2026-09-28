#pragma once
#include "control/control_types.h"
#include "protocol/scrp/envelope.h"
#include "transport/telemetry/winhttp_websocket.h"
#include <mutex>
#include <set>

namespace runner::observation {
// Internal adapter types, NOT a new JSON/wire contract. A trusted Host adapter
// must obtain these values from an agreed issuer. Control tokens are not defaults.
struct UploadGrant {
    scrp::SessionContext session;
    std::string connection_id, task_id, action_id, upload_id;
    telemetry::ChannelCredential credential;
    std::chrono::system_clock::time_point expires_at;
    std::size_t max_bytes = 0;
    std::uint64_t max_pixels = 0;
};
enum class AuthorizationMode { HeaderCredential, Host6055UploadId };
struct Destination {
    // Trusted deployment origin, e.g. https://host.example:17443 (no path).
    // The existing OBSERVE schema defines /scrp/v1/observations/<upload_id>.
    std::wstring origin;
    unsigned completed_status = 0; // Explicit agreed completion: 200, 201 or 204, never 202.
    std::chrono::milliseconds timeout{5000}; // Total operation budget, not per chunk.
    // Explicit opt-in to host_control 6055cc6: 256-bit upload_id is the secret
    // capability delivered by authenticated OBSERVE; no Authorization header.
    AuthorizationMode authorization = AuthorizationMode::HeaderCredential;
    telemetry::TlsTrust trust;
};
class HttpsUploader {
public:
    explicit HttpsUploader(Destination);
    // Serial use on GuiSession's uploader thread. A grant is burned before I/O;
    // failures are not retried. Throws fixed diagnostics, never URL/token/body.
    bool upload(const UploadGrant&, const scrp::Envelope&, const control::Observation&,
                const std::atomic<bool>& cancelled);
private:
    Destination destination_;
    std::wstring host_;
    unsigned short port_ = 0;
    std::mutex mutex_;
    std::set<std::string> consumed_;
};
} // namespace runner::observation
