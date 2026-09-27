#pragma once
#include "pending_event_store.h"
#include "winhttp_websocket.h"
#include <functional>
#include <memory>

namespace telemetry {
enum class ConnectionState { disconnected, connecting, wait_channel_ack, ready, reconnecting, stopping, stopped };
struct Context {
    scrp::SessionContext session;
    ConnectionSettings connection;
    // Called by the worker for every attempt; must return promptly. Scope must
    // match this immutable session/runtime/generation. A new generation needs a new client.
    std::function<ChannelCredential()> credential;
    std::shared_ptr<const scrp::TelemetrySchema> schema;
};
struct ClientOptions {
    Limits limits;
    std::chrono::milliseconds ack_timeout{2000};
    std::chrono::milliseconds hello_timeout{5000};
    std::chrono::milliseconds reconnect_base{1000};
    unsigned int max_reconnects = 4;
    std::size_t max_messages_per_connection = 65536; // bounds replay metadata
};
struct Snapshot {
    ConnectionState state;
    std::size_t pending_events;
    std::size_t pending_bytes;
    std::uint64_t expired_events;
    std::uint64_t rejected_events;
    std::uint64_t errors;
    std::string last_error; // Fixed diagnostics only; never raw peer data/credentials.
};

class TelemetryClient {
public:
    using SocketFactory=std::function<std::unique_ptr<WebSocket>()>;
    explicit TelemetryClient(Context context,ClientOptions options={},SocketFactory factory=make_winhttp_websocket);
    ~TelemetryClient();
    TelemetryClient(const TelemetryClient&)=delete;
    TelemetryClient& operator=(const TelemetryClient&)=delete;
    void start(); // one-shot; enqueue before start is supported
    EnqueueResult enqueue(const scrp::SecurityEvent& event); // no network I/O
    Snapshot snapshot() const;
    void stop(); // interrupt waits, cancel I/O, join; pending remains inspectable
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace telemetry
