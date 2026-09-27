#include "telemetry_client.h"
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace telemetry {
struct TelemetryClient::Impl {
    Context context;
    ClientOptions options;
    SocketFactory factory;
    mutable std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable wake;
    PendingEventStore pending;
    ConnectionState state=ConnectionState::disconnected;
    std::atomic<bool> stopping{false};
    bool started=false;
    std::thread worker;
    std::uint64_t expired=0,rejected=0,errors=0;
    std::string last_error;

    Impl(Context c,ClientOptions o,SocketFactory f):context(std::move(c)),options(o),factory(std::move(f)),pending(o.limits) {
        scrp::validate_session(context.session); validate_connection_settings(context.connection);
        if(!context.schema || !context.credential || !factory || o.ack_timeout.count()<=0 ||
           o.hello_timeout.count()<=0 || o.reconnect_base.count()<=0 || o.reconnect_base.count()>60000 ||
           o.max_reconnects>4 || o.max_messages_per_connection<2 || o.max_messages_per_connection>65536 ||
           o.limits.event_bytes>16384 || o.limits.event_bytes>context.connection.max_message_bytes ||
           o.limits.buffer_bytes>10*1024*1024 || o.limits.retention>std::chrono::minutes(5))
            throw std::invalid_argument("Invalid Telemetry context/options");
    }
    void set_state(ConnectionState next) { std::lock_guard<std::mutex> lock(mutex); if(!stopping) state=next; }
    void error(const char* text) { std::lock_guard<std::mutex> lock(mutex); ++errors; last_error=text; }
    void expire_locked() {
        auto ids=pending.expire(Clock::now());
        if(!ids.empty()) { expired+=ids.size(); ++errors; last_error="Pending events expired; monitoring coverage degraded"; }
    }
    void pause(std::chrono::milliseconds duration) {
        const auto end=Clock::now()+duration;
        std::unique_lock<std::mutex> lock(mutex);
        while(!stopping && Clock::now()<end) {
            expire_locked();
            wake.wait_until(lock,std::min(end,Clock::now()+std::chrono::milliseconds(20)));
        }
    }
    void run() noexcept {
        unsigned int retries=0;
        while(!stopping) {
            std::unique_ptr<WebSocket> socket;
            try {
                set_state(retries?ConnectionState::reconnecting:ConnectionState::connecting);
                auto credential=context.credential(); validate_credential(credential);
                socket=factory(); if(!socket) throw std::runtime_error("Missing socket");
                socket->connect(context.connection,credential,stopping);
                validate_credential(credential);
                auto hello=scrp::channel_hello(context.session,*context.schema);
                const auto hello_text=scrp::serialize(hello);
                if(hello_text.size()>context.connection.max_message_bytes) throw std::runtime_error("Oversized HELLO");
                socket->send(hello_text,stopping); set_state(ConnectionState::wait_channel_ack);
                auto deadline=Clock::now()+options.hello_timeout;
                std::string connection,inflight_event,inflight_message;
                std::uint64_t tx=1,rx=1;
                std::set<std::string> seen_ids,seen_nonces;
                bool ready=false;
                while(!stopping) {
                    if(std::chrono::system_clock::now()>=credential.expires_at) throw std::runtime_error("Credential expired");
                    {
                        std::lock_guard<std::mutex> lock(mutex); expire_locked();
                        if(!inflight_event.empty()) {
                            scrp::SecurityEvent front;
                            if(!pending.first(front) || front.event_id!=inflight_event)
                                throw std::runtime_error("In-flight event expired");
                        }
                    }
                    if((!ready || !inflight_event.empty()) && Clock::now()>=deadline) throw std::runtime_error("ACK timeout");
                    if(ready && inflight_event.empty()) {
                        scrp::SecurityEvent event; bool found;
                        { std::lock_guard<std::mutex> lock(mutex); found=pending.first(event); }
                        if(found) {
                            if(tx>=options.max_messages_per_connection) throw std::runtime_error("Connection message limit");
                            auto message=scrp::security_event(context.session,connection,++tx,event);
                            auto text=scrp::serialize(message);
                            if(text.size()>options.limits.event_bytes) throw std::runtime_error("Oversized event");
                            // Bind ACK to this attempt before entering receive. A send failure
                            // preserves the pending payload and obtains a fresh envelope next time.
                            inflight_event=event.event_id; inflight_message=message.message_id;
                            socket->send(text,stopping); deadline=Clock::now()+options.ack_timeout;
                        }
                    }
                    std::string incoming;
                    if(!socket->receive(incoming,std::chrono::milliseconds(20),stopping)) continue;
                    auto ack=scrp::parse_envelope(incoming,context.connection.max_message_bytes);
                    scrp::validate_incoming(ack,context.session,rx);
                    if(rx>options.max_messages_per_connection || !seen_ids.insert(ack.message_id).second ||
                       !seen_nonces.insert(ack.nonce).second || !ack.task_id.isNull() || !ack.action_id.isNull())
                        throw std::runtime_error("Invalid ACK identity");
                    if(!ready) {
                        if(ack.type!="CHANNEL_ACK" || ack.correlation_id!=hello.message_id)
                            throw std::runtime_error("Invalid CHANNEL_ACK");
                        connection=context.schema->channel_ack(ack);
                        if(connection.empty() || !ack.connection_id.isString() || ack.connection_id.asString()!=connection ||
                           scrp::json_text(Json::Value(connection)).size()>258)
                            throw std::runtime_error("Invalid connection binding");
                        ready=true; set_state(ConnectionState::ready);
                    } else {
                        if(ack.type!="EVENT_ACK" || ack.connection_id!=connection || inflight_event.empty() ||
                           ack.correlation_id!=inflight_message || context.schema->event_ack(ack)!=inflight_event)
                            throw std::runtime_error("Invalid EVENT_ACK");
                        std::lock_guard<std::mutex> lock(mutex);
                        if(!pending.acknowledge(inflight_event)) throw std::runtime_error("Unknown pending event");
                        inflight_event.clear(); inflight_message.clear(); retries=0;
                    }
                    ++rx;
                }
            } catch(const SocketError& e) {
                if(!stopping) error(e.what());
            } catch(...) {
                if(!stopping) error("Telemetry connection/ACK failed; unacknowledged events retained");
            }
            if(socket) socket->close();
            if(stopping) break;
            if(retries>=options.max_reconnects) {
                set_state(ConnectionState::disconnected);
                { std::lock_guard<std::mutex> lock(mutex); ++errors; last_error="Telemetry recovery exhausted: "+last_error; }
                while(!stopping) pause(std::chrono::milliseconds(20));
                break;
            }
            set_state(ConnectionState::reconnecting);
            // Random jitter without retaining/logging authentication material.
            unsigned int jitter=0;
            try { for(char c:scrp::random_nonce()) jitter=jitter*33+static_cast<unsigned char>(c); } catch(...) {}
            auto delay=options.reconnect_base*(1u<<retries++);
            pause(delay+std::chrono::milliseconds(jitter%(static_cast<unsigned int>(delay.count()/4)+1)));
        }
        std::lock_guard<std::mutex> lock(mutex);
        if(pending.size()) { ++errors; last_error="Stopped with unacknowledged events; memory-only retention"; }
        state=ConnectionState::stopped;
    }
};
TelemetryClient::TelemetryClient(Context c,ClientOptions o,SocketFactory f):impl_(std::make_unique<Impl>(std::move(c),o,std::move(f))) {}
TelemetryClient::~TelemetryClient() { stop(); }
void TelemetryClient::start() {
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->started || impl_->stopping) throw std::logic_error("TelemetryClient is one-shot");
    impl_->worker=std::thread([this]{impl_->run();}); impl_->started=true;
}
EnqueueResult TelemetryClient::enqueue(const scrp::SecurityEvent& event) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->stopping) return EnqueueResult::stopped;
    impl_->expire_locked();
    try {
        scrp::validate_event_identity(event); impl_->context.schema->validate_event(event);
        // Reserve the maximum supported connection string and sequence width.
        auto envelope=scrp::security_event(impl_->context.session,std::string(256,'c'),std::numeric_limits<std::uint64_t>::max(),event);
        const auto serialized=scrp::serialize(envelope);
        if(serialized.size()>impl_->options.limits.event_bytes) {
            ++impl_->rejected; ++impl_->errors; impl_->last_error="Event exceeds serialized size limit; coverage degraded";
            return EnqueueResult::event_too_large;
        }
        scrp::parse_envelope(serialized,impl_->context.connection.max_message_bytes);
        const auto result=impl_->pending.add(event,serialized.size(),Clock::now());
        if(result!=EnqueueResult::accepted && result!=EnqueueResult::duplicate) {
            ++impl_->rejected; ++impl_->errors; impl_->last_error="Event rejected by pending size/identity limits; coverage degraded";
        }
        impl_->wake.notify_all(); return result;
    } catch(...) {
        ++impl_->rejected; ++impl_->errors; impl_->last_error="Invalid Telemetry event or schema validation failure";
        return EnqueueResult::invalid;
    }
}
Snapshot TelemetryClient::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->state,impl_->pending.size(),impl_->pending.bytes(),impl_->expired,impl_->rejected,impl_->errors,impl_->last_error};
}
void TelemetryClient::stop() {
    std::lock_guard<std::mutex> lifecycle(impl_->lifecycle);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(impl_->state==ConnectionState::stopped) return;
        impl_->stopping=true; impl_->state=ConnectionState::stopping;
    }
    impl_->wake.notify_all();
    if(impl_->worker.joinable()) impl_->worker.join();
    else {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if(impl_->pending.size()) { ++impl_->errors; impl_->last_error="Stopped with unacknowledged events; memory-only retention"; }
        impl_->state=ConnectionState::stopped;
    }
}
} // namespace telemetry
