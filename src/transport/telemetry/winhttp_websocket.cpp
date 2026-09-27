#include "winhttp_websocket.h"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <array>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace telemetry {
namespace {
void check(bool ok,const char* message) { if(!ok) throw SocketError(std::string(message)+" (Win32 "+std::to_string(GetLastError())+")"); }
struct Url { std::wstring host,path; INTERNET_PORT port; };
Url endpoint(const std::wstring& input) {
    check(input.compare(0,6,L"wss://")==0 && input.find_first_of(L"?#@\r\n\\")==std::wstring::npos &&
          input.find(L'\0')==std::wstring::npos, "Invalid Telemetry WSS endpoint");
    const auto https=L"https://"+input.substr(6);
    URL_COMPONENTS c{}; c.dwStructSize=sizeof(c); c.dwHostNameLength=DWORD(-1); c.dwUrlPathLength=DWORD(-1);
    check(WinHttpCrackUrl(https.c_str(),static_cast<DWORD>(https.size()),0,&c), "Invalid Telemetry URL");
    check(c.dwHostNameLength>0 && c.dwUrlPathLength>0, "Telemetry endpoint needs host and path");
    return {std::wstring(c.lpszHostName,c.dwHostNameLength),std::wstring(c.lpszUrlPath,c.dwUrlPathLength),c.nPort};
}
struct AsyncHandle {
    HINTERNET value=nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    DWORD completed=0;
    DWORD error=0;
    WINHTTP_WEB_SOCKET_STATUS read{};
    bool closing=false;
    bool callback_set=false;
    ~AsyncHandle() { close(); }
    static void CALLBACK callback(HINTERNET,DWORD_PTR context,DWORD status,void* info,DWORD size) noexcept {
        if(!context) return;
        auto* self=reinterpret_cast<AsyncHandle*>(context);
        std::lock_guard<std::mutex> lock(self->mutex);
        if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
            self->error=size>=sizeof(WINHTTP_ASYNC_RESULT)?static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError:ERROR_WINHTTP_CONNECTION_ERROR;
        if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE && size>=sizeof(WINHTTP_WEB_SOCKET_STATUS))
            self->read=*static_cast<WINHTTP_WEB_SOCKET_STATUS*>(info);
        if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) self->closing=true;
        self->completed|=status;
        self->changed.notify_all();
    }
    void attach(HINTERNET h,bool inherited_callback=false) {
        value=h; check(value!=nullptr,"Cannot allocate WinHTTP handle");
        callback_set=inherited_callback;
        const DWORD_PTR context=reinterpret_cast<DWORD_PTR>(this);
        check(WinHttpSetOption(value,WINHTTP_OPTION_CONTEXT_VALUE,const_cast<DWORD_PTR*>(&context),sizeof(context)),"Cannot set WinHTTP context");
        check(WinHttpSetStatusCallback(value,callback,WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS,0)!=WINHTTP_INVALID_STATUS_CALLBACK,"Cannot set WinHTTP callback");
        callback_set=true;
    }
    void close() noexcept {
        if(!value) return;
        WinHttpCloseHandle(value); value=nullptr;
        if(callback_set) {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock,[&]{return closing;});
        }
    }
    bool wait(DWORD flag,std::chrono::milliseconds timeout,const std::atomic<bool>& stop) {
        const auto end=std::chrono::steady_clock::now()+timeout;
        std::unique_lock<std::mutex> lock(mutex);
        while(!(completed&flag) && !error && !stop) {
            const auto now=std::chrono::steady_clock::now(); if(now>=end) return false;
            changed.wait_until(lock,std::min(end,now+std::chrono::milliseconds(20)));
        }
        check(!stop,"Telemetry stopped");
        if(error) throw SocketError("WinHTTP asynchronous operation failed (Win32 "+std::to_string(error)+")");
        completed&=~flag; return true;
    }
};
class WinHttpSocket final:public WebSocket {
    HINTERNET session_=nullptr,connection_=nullptr;
    std::unique_ptr<AsyncHandle> request_,socket_;
    ConnectionSettings settings_;
    std::array<char,4096> read_buffer_{};
    std::string assembled_;
    // These buffers outlive async operations, including failure/close callbacks.
    std::string outgoing_;
    std::wstring headers_;
    bool reading_=false;
    bool fragment_active_=false;
    std::chrono::steady_clock::time_point fragment_started_{};
    static void async_started(BOOL ok) {
        check(ok || GetLastError()==ERROR_IO_PENDING,"WinHTTP request failed");
    }
    void wait(AsyncHandle& h,DWORD flag) {
        check(h.wait(flag,settings_.io_timeout,*stop_),"WinHTTP operation timed out");
    }
    const std::atomic<bool>* stop_=nullptr;
public:
    ~WinHttpSocket() override { close(); }
    void connect(const ConnectionSettings& settings,const ChannelCredential& credential,const std::atomic<bool>& stop) override {
        close(); validate_connection_settings(settings); validate_credential(credential);
        settings_=settings; stop_=&stop;
        const auto url=endpoint(settings.endpoint);
        session_=WinHttpOpen(L"SandboxRunner/Telemetry",WINHTTP_ACCESS_TYPE_NO_PROXY,
                             WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
        check(session_!=nullptr,"Cannot open WinHTTP session");
        DWORD protocols=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        check(WinHttpSetOption(session_,WINHTTP_OPTION_SECURE_PROTOCOLS,&protocols,sizeof(protocols)),"Cannot require TLS 1.2");
        const int timeout=static_cast<int>(settings.io_timeout.count());
        check(WinHttpSetTimeouts(session_,timeout,timeout,timeout,timeout),"Cannot configure WinHTTP deadlines");
        connection_=WinHttpConnect(session_,url.host.c_str(),url.port,0);
        check(connection_!=nullptr,"Cannot create WinHTTP connection");
        request_=std::make_unique<AsyncHandle>();
        request_->attach(WinHttpOpenRequest(connection_,L"GET",url.path.c_str(),nullptr,
                                          WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
        DWORD disabled=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
        check(WinHttpSetOption(request_->value,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled)),"Cannot restrict HTTP upgrade");
        check(WinHttpSetOption(request_->value,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0),"Cannot request WebSocket upgrade");
        headers_=credential.header_name+L": "+credential.header_value+L"\r\n";
        async_started(WinHttpSendRequest(request_->value,headers_.c_str(),static_cast<DWORD>(headers_.size()),nullptr,0,0,
                                        reinterpret_cast<DWORD_PTR>(request_.get())));
        wait(*request_,WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE);
        async_started(WinHttpReceiveResponse(request_->value,nullptr));
        wait(*request_,WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE);
        DWORD status=0,size=sizeof(status);
        check(WinHttpQueryHeaders(request_->value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX) && status==101,
              "Telemetry HTTP upgrade rejected");
        // Default Schannel chain/hostname validation remains enabled. A pin cannot
        // authorize a self-signed or wrong-host certificate on its own.
        if(!settings.trust.leaf_sha256.empty()) {
            PCCERT_CONTEXT cert=nullptr; size=sizeof(cert);
            check(WinHttpQueryOption(request_->value,WINHTTP_OPTION_SERVER_CERT_CONTEXT,&cert,&size),"Cannot inspect TLS certificate");
            std::array<BYTE,32> hash{}; DWORD hash_size=static_cast<DWORD>(hash.size());
            const bool hashed=CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM,0,nullptr,cert->pbCertEncoded,cert->cbCertEncoded,hash.data(),&hash_size);
            CertFreeCertificateContext(cert);
            check(hashed && hash_size==hash.size(),"Cannot hash TLS certificate");
            std::string hex; for(auto b:hash) { hex+="0123456789abcdef"[b>>4]; hex+="0123456789abcdef"[b&15]; }
            check(hex==settings.trust.leaf_sha256,"TLS certificate pin mismatch");
        }
        socket_=std::make_unique<AsyncHandle>();
        socket_->attach(WinHttpWebSocketCompleteUpgrade(request_->value,reinterpret_cast<DWORD_PTR>(socket_.get())),true);
        request_.reset(); headers_.clear();
    }
    void send(const std::string& text,const std::atomic<bool>& stop) override {
        check(socket_ && text.size()<=settings_.max_message_bytes,"Invalid WebSocket send");
        outgoing_=text;
        const auto result=WinHttpWebSocketSend(socket_->value,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                               &outgoing_[0],static_cast<DWORD>(outgoing_.size()));
        check(result==NO_ERROR || result==ERROR_IO_PENDING,"WebSocket send failed");
        check(socket_->wait(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,settings_.io_timeout,stop),"WebSocket send timed out");
        outgoing_.clear();
    }
    bool receive(std::string& text,std::chrono::milliseconds poll,const std::atomic<bool>& stop) override {
        check(socket_!=nullptr,"WebSocket not connected");
        if(!reading_) {
            const auto result=WinHttpWebSocketReceive(socket_->value,read_buffer_.data(),static_cast<DWORD>(read_buffer_.size()),nullptr,nullptr);
            check(result==NO_ERROR || result==ERROR_IO_PENDING,"WebSocket receive failed"); reading_=true;
        }
        if(!socket_->wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE,poll,stop)) {
            check(!fragment_active_ || std::chrono::steady_clock::now()-fragment_started_<settings_.io_timeout,"WebSocket fragmented message timed out");
            return false;
        }
        reading_=false;
        WINHTTP_WEB_SOCKET_STATUS read;
        { std::lock_guard<std::mutex> lock(socket_->mutex); read=socket_->read; }
        check(read.eBufferType==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || read.eBufferType==WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE,
              "WebSocket closed or non-text message received");
        check(read.dwBytesTransferred<=read_buffer_.size() && read.dwBytesTransferred<=settings_.max_message_bytes-assembled_.size(),"WebSocket message exceeds limit");
        if(!fragment_active_) { fragment_started_=std::chrono::steady_clock::now(); fragment_active_=true; }
        assembled_.append(read_buffer_.data(),read.dwBytesTransferred);
        check(std::chrono::steady_clock::now()-fragment_started_<settings_.io_timeout,"WebSocket fragmented message timed out");
        if(read.eBufferType==WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) return false;
        text=std::move(assembled_); assembled_.clear(); fragment_active_=false; return true;
    }
    void close() noexcept override {
        // Closing asynchronous handles cancels I/O. State/buffers survive until
        // HANDLE_CLOSING, so stop never races an in-progress synchronous receive.
        socket_.reset(); request_.reset();
        if(connection_) { WinHttpCloseHandle(connection_); connection_=nullptr; }
        if(session_) { WinHttpCloseHandle(session_); session_=nullptr; }
        reading_=false; fragment_active_=false; assembled_.clear(); outgoing_.clear(); headers_.clear();
    }
};
}
void validate_connection_settings(const ConnectionSettings& s) {
    endpoint(s.endpoint);
    check(s.endpoint.size()<=4096 && s.io_timeout.count()>0 && s.io_timeout.count()<=60000 &&
          s.max_message_bytes>0 && s.max_message_bytes<=65536,"Invalid Telemetry connection limits");
    check(s.trust.leaf_sha256.empty() || (s.trust.leaf_sha256.size()==64 &&
        s.trust.leaf_sha256.find_first_not_of("0123456789abcdef")==std::string::npos),"Invalid TLS certificate pin");
}
void validate_credential(const ChannelCredential& c) {
    check(!c.header_name.empty() && !c.header_value.empty() && c.header_name.size()<=128 && c.header_value.size()<=8192 &&
          c.expires_at>std::chrono::system_clock::now(),"Missing or expired Telemetry credential");
    std::wstring lower;
    for(wchar_t ch:c.header_name) {
        check((ch>=L'A'&&ch<=L'Z')||(ch>=L'a'&&ch<=L'z')||(ch>=L'0'&&ch<=L'9')||ch==L'-',"Invalid credential header name");
        lower+=ch>=L'A'&&ch<=L'Z'?ch+32:ch;
    }
    check(lower!=L"host" && lower!=L"connection" && lower!=L"upgrade" && lower!=L"content-length" &&
          lower!=L"transfer-encoding" && lower!=L"cookie" && lower.compare(0,14,L"sec-websocket-")!=0,
          "Credential cannot replace WebSocket headers");
    for(wchar_t ch:c.header_value) check(ch>=32 && ch<=126,"Invalid credential header value");
}
std::unique_ptr<WebSocket> make_winhttp_websocket() { return std::make_unique<WinHttpSocket>(); }
} // namespace telemetry
