#include "runner_lifecycle.h"

namespace runner {
std::unique_ptr<telemetry::TelemetryClient> start_telemetry(
    SessionContext context, HANDLE stop_event, std::chrono::milliseconds ready_timeout,
    telemetry::ClientOptions options, telemetry::TelemetryClient::SocketFactory factory) {
    if (!stop_event || ready_timeout.count() <= 0 || ready_timeout > std::chrono::minutes(5))
        throw std::invalid_argument("Invalid Runner startup wait settings");
    const auto initial_wait = WaitForSingleObject(stop_event, 0);
    if (initial_wait == WAIT_OBJECT_0) throw std::runtime_error("Runner startup cancelled");
    if (initial_wait != WAIT_TIMEOUT) throw std::runtime_error("Runner stop event wait failed");
    validate_session_context(context);
    auto client = std::make_unique<telemetry::TelemetryClient>(std::move(context), options, std::move(factory));
    const auto deadline = std::chrono::steady_clock::now() + ready_timeout;
    client->start();
    // Handshake and retries remain entirely inside TelemetryClient. A bounded,
    // cancellable snapshot wait does not introduce another worker or protocol.
    for (;;) {
        const auto wait = WaitForSingleObject(stop_event, 0);
        if (wait == WAIT_OBJECT_0) throw std::runtime_error("Runner startup cancelled");
        if (wait != WAIT_TIMEOUT) throw std::runtime_error("Runner stop event wait failed");
        const auto snapshot = client->snapshot();
        if (snapshot.state == telemetry::ConnectionState::ready) return client;
        if (snapshot.state == telemetry::ConnectionState::disconnected && snapshot.errors)
            throw std::runtime_error("Telemetry startup failed: recovery exhausted");
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Telemetry startup failed: READY timeout");
        WaitForSingleObject(stop_event, 20);
    }
}
} // namespace runner
