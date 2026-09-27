#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <stdexcept>

namespace telemetry {
// Contains only local operation names and numeric errors, never peer/header text.
class SocketError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
// Supplied by the Host's agreed authentication contract; no built-in bearer scheme.
struct ChannelCredential {
    std::wstring header_name;
    std::wstring header_value;
    std::chrono::system_clock::time_point expires_at;
};
struct TlsTrust {
    // Normal Windows certificate chain + hostname verification is always required.
    // Optional SHA-256 leaf pin is an ADDITIONAL check, never a trust bypass.
    std::string leaf_sha256;
};
struct ConnectionSettings {
    std::wstring endpoint;
    TlsTrust trust;
    std::chrono::milliseconds io_timeout{5000};
    std::size_t max_message_bytes = 65536;
};
void validate_connection_settings(const ConnectionSettings& settings);
void validate_credential(const ChannelCredential& credential);

class WebSocket {
public:
    virtual ~WebSocket() = default;
    virtual void connect(const ConnectionSettings&,const ChannelCredential&,const std::atomic<bool>& stop)=0;
    virtual void send(const std::string&,const std::atomic<bool>& stop)=0;
    // false means no complete message yet; a pending asynchronous read is retained.
    virtual bool receive(std::string&,std::chrono::milliseconds poll,const std::atomic<bool>& stop)=0;
    virtual void close() noexcept=0;
};
std::unique_ptr<WebSocket> make_winhttp_websocket();
} // namespace telemetry
