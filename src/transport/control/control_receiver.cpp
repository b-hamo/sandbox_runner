#include "control_receiver.h"
#include <set>
#include <stdexcept>
#include <utility>
#include <algorithm>

namespace control {
namespace {
bool request_type(const std::string& type) {
    return type == "OBSERVE" || type == "ACTION_REQUEST" || type == "STATE_REQUEST" ||
           type == "HEARTBEAT" || type == "ARTIFACT_REQUEST" || type == "TERMINATE";
}
void require(bool value) {
    if (!value) throw std::runtime_error("Control protocol rejected");
}
struct CloseSocket {
    telemetry::WebSocket& socket;
    ~CloseSocket() { socket.close(); }
};
ReplyHandlers wrap(Handlers handlers) {
    ReplyHandlers result;
    for (auto& entry : handlers) {
        if (!entry.second) throw std::invalid_argument("Invalid Control request handler");
        result.emplace(entry.first, [handler=std::move(entry.second)](const scrp::Envelope& e) {
            handler(e); return Replies{};
        });
    }
    return result;
}
bool reply_type(const std::string& request, const std::string& reply) {
    if (reply == "ERROR") return true;
    if (request == "ACTION_REQUEST") return reply == "ACK" || reply == "ACTION_RESULT";
    if (request == "HEARTBEAT") return reply == "ALIVE";
    if (request == "OBSERVE") return reply == "OBSERVE_RESULT";
    if (request == "STATE_REQUEST") return reply == "STATE_RESULT";
    if (request == "TERMINATE") return reply == "TERMINATE_RESULT";
    return request == "ARTIFACT_REQUEST" && reply == "ARTIFACT_RESULT";
}
}

Receiver::Receiver(Context context, Handlers handlers, Options options, SocketFactory factory)
    : Receiver(std::move(context), wrap(std::move(handlers)), options, std::move(factory)) {}

Receiver::Receiver(Context context, ReplyHandlers handlers, Options options, SocketFactory factory)
    : context_(std::move(context)), handlers_(std::move(handlers)), options_(options), factory_(std::move(factory)) {
    scrp::validate_session(context_.session);
    telemetry::validate_connection_settings(context_.connection);
    if (!context_.schema || !context_.credential || !factory_ || handlers_.empty() ||
        options_.hello_timeout.count() <= 0 || options_.hello_timeout > std::chrono::seconds(60) ||
        options_.max_messages < 2 || options_.max_messages > 65536)
        throw std::invalid_argument("Invalid Control receiver configuration");
    for (const auto& entry : handlers_)
        if (!request_type(entry.first) || !entry.second)
            throw std::invalid_argument("Invalid Control request handler");
}

void Receiver::state(State value, const char* diagnostic) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.state = value;
    snapshot_.diagnostic = diagnostic;
}
Snapshot Receiver::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void Receiver::run(const std::atomic<bool>& stop) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) throw std::logic_error("Control receiver is single use");
        started_ = true;
    }
    const char* diagnostic = "Control connection failed";
    try {
        if (stop) { state(State::stopped); return; }
        state(State::connecting);
        auto credential = context_.credential();
        telemetry::validate_credential(credential);
        auto socket = factory_();
        require(bool(socket));
        CloseSocket closer{*socket};
        socket->connect(context_.connection, credential, stop);
        telemetry::validate_credential(credential);
        if (stop) throw std::runtime_error("Control stopped");

        diagnostic = "Control handshake failed";
        auto hello = scrp::make_envelope(context_.session, Json::Value(), 1, "HELLO",
                                         context_.schema->hello(context_.session));
        const auto text = scrp::serialize(hello);
        require(text.size() <= context_.connection.max_message_bytes);
        const auto deadline = std::chrono::steady_clock::now() + options_.hello_timeout;
        socket->send(text, stop);
        state(State::wait_hello_ack);
        std::string connection;
        std::uint64_t sequence = 1, outbound = 1;
        std::size_t message_limit = context_.connection.max_message_bytes;
        std::set<std::string> message_ids, nonces, actions;
        while (!stop) {
            require(std::chrono::system_clock::now() < credential.expires_at);
            if (connection.empty()) require(std::chrono::steady_clock::now() < deadline);
            std::string incoming;
            if (!socket->receive(incoming, std::chrono::milliseconds(20), stop)) continue;
            if (stop) break;
            require(std::chrono::system_clock::now() < credential.expires_at);
            if (connection.empty()) require(std::chrono::steady_clock::now() < deadline);
            const auto request = scrp::parse_envelope(incoming, message_limit);
            scrp::validate_incoming(request, context_.session, sequence);
            require(sequence <= options_.max_messages);
            require(message_ids.insert(request.message_id).second && nonces.insert(request.nonce).second);
            if (connection.empty()) {
                require(request.type == "HELLO_ACK" && request.correlation_id == hello.message_id &&
                        request.task_id.isNull() && request.action_id.isNull() && request.error.isNull());
                connection = context_.schema->hello_ack(request);
                require(!connection.empty() && request.connection_id.isString() &&
                        request.connection_id.asString() == connection);
                message_limit = std::min(message_limit, context_.schema->message_limit());
                require(message_limit > 0);
                state(State::receiving);
                diagnostic = "Control request rejected or connection lost";
            } else {
                require(request.connection_id == connection && request_type(request.type) &&
                        request.correlation_id.isNull() && request.status.isNull() && request.error.isNull());
                const auto handler = handlers_.find(request.type);
                require(handler != handlers_.end());
                if (request.type == "ACTION_REQUEST" || request.type == "OBSERVE")
                    require(request.task_id.isString() && request.action_id.isString());
                context_.schema->validate_request(request);
                // Bound total action-ID storage as well as count. IDs are otherwise
                // opaque; rejecting repeats also rejects conflicting payloads.
                if (request.type == "ACTION_REQUEST" || request.type == "OBSERVE") {
                    require(request.action_id.asString().size() <= 256);
                    require(actions.insert(request.action_id.asString()).second);
                }
                if (stop) break;
                diagnostic = "Control request handler failed";
                auto replies = handler->second(request);
                require(replies.messages.size() <= 2);
                std::vector<std::string> wire;
                for (const auto& reply : replies.messages) {
                    require(reply_type(request.type, reply.type) && outbound < options_.max_messages);
                    auto e = scrp::make_envelope(context_.session, connection, ++outbound, reply.type, reply.payload);
                    e.correlation_id = request.message_id;
                    e.task_id = request.task_id; e.action_id = request.action_id;
                    e.status = reply.status; e.error = reply.error;
                    context_.schema->validate_reply(e);
                    auto serialized = scrp::serialize(e);
                    require(serialized.size() <= message_limit);
                    wire.push_back(std::move(serialized));
                }
                diagnostic = "Control response send failed";
                for (const auto& serialized : wire) socket->send(serialized, stop);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++snapshot_.dispatched;
                }
                diagnostic = "Control request rejected or connection lost";
                if (replies.finish) break;
            }
            ++sequence;
        }
        // Close before publishing a terminal state (also on exceptions).
    } catch (...) {
        state(stop ? State::stopped : State::failed, stop ? "" : diagnostic);
        return;
    }
    state(State::stopped);
}
} // namespace control
