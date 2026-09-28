#pragma once
#include "protocol/scrp/envelope.h"
#include "transport/telemetry/winhttp_websocket.h"
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace control {
// Closed payload schemas belong to the Host contract (W1 does not freeze them).
// Implementations must throw on invalid input and return promptly. No GUI or I/O.
class Schema {
public:
    virtual ~Schema() = default;
    virtual Json::Value hello(const scrp::SessionContext&) const = 0;
    virtual std::string hello_ack(const scrp::Envelope&) const = 0;
    virtual void validate_request(const scrp::Envelope&) const = 0;
    virtual void validate_reply(const scrp::Envelope&) const {
        throw std::logic_error("Control reply schema not supplied");
    }
    virtual std::size_t message_limit() const { return 65536; }
};

struct Context {
    scrp::SessionContext session;
    telemetry::ConnectionSettings connection;
    std::function<telemetry::ChannelCredential()> credential;
    std::shared_ptr<const Schema> schema;
};
struct Options {
    std::chrono::milliseconds hello_timeout{5000};
    std::size_t max_messages = 65536; // includes HELLO_ACK; bounded replay records
};
enum class State { idle, connecting, wait_hello_ack, receiving, stopped, failed };
struct Snapshot {
    State state = State::idle;
    std::size_t dispatched = 0;
    std::string diagnostic; // fixed text only, never peer data or credentials
};

// Hooks run synchronously on the run() caller's thread and MUST return promptly.
// Request handlers should hand off to bounded queues, not execute GUI operations.
// Throw on failed handoff. Dispatch is NOT an ACK or execution success.
using Handler = std::function<void(const scrp::Envelope&)>;
using Handlers = std::map<std::string, Handler>;
struct Reply {
    std::string type;
    Json::Value payload{Json::objectValue};
    std::string status = "OK";
    Json::Value error;
};
struct Replies {
    std::vector<Reply> messages; // at most ACK + result, no delayed/asynchronous use
    bool finish = false; // close only after all responses have been sent
};
using ReplyHandler = std::function<Replies(const scrp::Envelope&)>;
using ReplyHandlers = std::map<std::string, ReplyHandler>;

class Receiver {
public:
    using SocketFactory = std::function<std::unique_ptr<telemetry::WebSocket>()>;
    Receiver(Context context, Handlers handlers, Options options = {},
             SocketFactory factory = telemetry::make_winhttp_websocket);
    Receiver(Context context, ReplyHandlers handlers, Options options = {},
             SocketFactory factory = telemetry::make_winhttp_websocket);
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Single use, blocking, owns no worker. Owner sets stop and joins its thread
    // before destroying Receiver. Repeated/concurrent run calls throw.
    // No automatic reconnect: bootstrap credentials must not be reused.
    void run(const std::atomic<bool>& stop);
    Snapshot snapshot() const;
private:
    void state(State value, const char* diagnostic = "");
    Context context_;
    ReplyHandlers handlers_;
    Options options_;
    SocketFactory factory_;
    mutable std::mutex mutex_;
    Snapshot snapshot_;
    bool started_ = false;
};
} // namespace control
