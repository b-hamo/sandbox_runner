#include "gui_product.h"
#include "control_session.h"
#include <iostream>

namespace runner {
namespace {
void check_stop(HANDLE stop) {
    if (!stop || WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
        throw std::runtime_error("GUI product startup cancelled or stop handle invalid");
}
}
DWORD run_product_control_session(::control::Context context, HANDLE stop,
                                  const GuiProductServices* services) {
    check_stop(stop);
    if (!services) {
        std::cout << "Control management mode: GUI Host authorization/upload contract is not configured\n";
        return run_control_session(std::move(context), stop);
    }
    const auto& binding = services->runtime.binding;
    if (binding.session_id != context.session.session_id || binding.runtime_id != context.session.runtime_id ||
        binding.generation != context.session.generation || services->runtime.worker.backend ||
        !services->prepare || !services->authorize || !services->take_upload_grant)
        throw std::invalid_argument("Invalid GUI product services or session binding");
    scrp::validate_session(context.session);
    telemetry::validate_connection_settings(context.connection);
    if (!context.credential) throw std::invalid_argument("Missing Control credential provider");
    try { telemetry::validate_credential(context.credential()); }
    catch (...) { throw std::invalid_argument("Invalid Control credential"); }
    // Construct transport before capturing. Reverse destruction stops all GUI
    // work before the uploader and the borrowed services can be released.
    observation::HttpsUploader uploader(services->observations);
    GuiSession gui(services->runtime, services->authorize,
        [&](const scrp::Envelope& request, const control::Observation& png, const std::atomic<bool>& cancelled) {
            if (cancelled) return false;
            const auto grant = services->take_upload_grant(request);
            return uploader.upload(grant, request, png, cancelled);
        });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto probe = gui.startup_probe();
    check_stop(stop);
    if (!probe.ok) throw std::runtime_error("GUI product startup capture failed");
    try { services->prepare(probe, stop, deadline); }
    catch (...) { throw std::runtime_error("GUI Host startup verification failed"); }
    check_stop(stop);
    if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("GUI Host startup verification timed out");
    // prepare must not turn local capture into approval. connected() still
    // demands a verified HostGrant after the authenticated HELLO_ACK.
    return run_control_session(std::move(context), stop, &gui);
}
} // namespace runner
