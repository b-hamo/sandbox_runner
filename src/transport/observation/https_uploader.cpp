#include "https_uploader.h"
#include "control/control_util.h"
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <condition_variable>

namespace runner::observation {
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Handle {
    HINTERNET value = nullptr;
    ~Handle() { if (value) WinHttpCloseHandle(value); }
};
// Callback state and caller-owned PNG/header buffers remain alive until the
// HANDLE_CLOSING callback, including cancellation, timeout and exception paths.
struct Request {
    HINTERNET value = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    DWORD completed = 0, error = 0, written = 0, read = 0;
    bool callback_set = false, closing = false;
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD size) noexcept {
        if (!context) return;
        auto& self = *reinterpret_cast<Request*>(context);
        std::lock_guard<std::mutex> lock(self.mutex);
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
            self.error = size >= sizeof(WINHTTP_ASYNC_RESULT)
                ? static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError : ERROR_WINHTTP_CONNECTION_ERROR;
        if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE && size == sizeof(DWORD))
            self.written = *static_cast<DWORD*>(info);
        if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) self.read = size;
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) self.closing = true;
        self.completed |= status;
        self.changed.notify_all();
    }
    void attach(HINTERNET handle) {
        value = handle;
        require(value != nullptr, "Cannot create observation HTTPS request");
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        require(WinHttpSetOption(value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)),
                "Cannot set observation HTTPS context");
        require(WinHttpSetStatusCallback(value, callback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0)
                != WINHTTP_INVALID_STATUS_CALLBACK, "Cannot set observation HTTPS callback");
        callback_set = true;
    }
    ~Request() {
        if (!value) return;
        WinHttpCloseHandle(value);
        if (callback_set) {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [this] { return closing; });
        }
    }
    void wait(DWORD flag, std::chrono::steady_clock::time_point deadline,
              std::chrono::system_clock::time_point expiry, const std::atomic<bool>& cancelled) {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            require(!cancelled, "Observation upload cancelled");
            const auto now = std::chrono::steady_clock::now();
            require(now < deadline && std::chrono::system_clock::now() < expiry,
                    "Observation upload expired or timed out");
            require(!error, "Observation HTTPS operation failed");
            if (completed & flag) { completed &= ~flag; return; }
            changed.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)));
        }
    }
};
void started(BOOL result) {
    require(result || GetLastError() == ERROR_IO_PENDING, "Cannot start observation HTTPS operation");
}
bool upload_id(const std::string& id) {
    return id.size() >= 16 && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
}
HttpsUploader::HttpsUploader(Destination destination) : destination_(std::move(destination)) {
    const auto& origin = destination_.origin;
    require(origin.size() <= 2048 && origin.compare(0, 8, L"https://") == 0 &&
            origin.find_first_of(L"/?#@\\\r\n\t ", 8) == std::wstring::npos &&
            origin.find(L'\0') == std::wstring::npos, "Invalid observation HTTPS origin");
    URL_COMPONENTS url{}; url.dwStructSize = sizeof(url); url.dwHostNameLength = DWORD(-1);
    require(WinHttpCrackUrl(origin.c_str(), static_cast<DWORD>(origin.size()), 0, &url) &&
            url.nScheme == INTERNET_SCHEME_HTTPS && url.dwHostNameLength > 0 && url.nPort > 0,
            "Invalid observation HTTPS origin");
    host_.assign(url.lpszHostName, url.dwHostNameLength); port_ = url.nPort;
    require(destination_.completed_status == 200 || destination_.completed_status == 201 ||
            destination_.completed_status == 204, "Missing observation upload completion contract");
    require(destination_.timeout.count() > 0 && destination_.timeout <= std::chrono::seconds(30),
            "Invalid observation upload deadline");
    require(destination_.authorization == AuthorizationMode::HeaderCredential ||
            (destination_.authorization == AuthorizationMode::Host6055UploadId && destination_.completed_status == 201),
            "Invalid observation authorization contract");
    require(destination_.trust.leaf_sha256.empty() || (destination_.trust.leaf_sha256.size()==64 &&
            destination_.trust.leaf_sha256.find_first_not_of("0123456789abcdef")==std::string::npos),
            "Invalid observation TLS pin");
}
bool HttpsUploader::upload(const UploadGrant& grant, const scrp::Envelope& request,
                          const control::Observation& observation, const std::atomic<bool>& cancelled) {
    std::unique_lock<std::mutex> serial(mutex_, std::try_to_lock);
    require(serial.owns_lock(), "Concurrent observation uploads are not supported");
    require(!cancelled, "Observation upload cancelled");
    scrp::validate_session(grant.session);
    require(request.type == "OBSERVE" && request.session.session_id == grant.session.session_id &&
            request.session.runtime_id == grant.session.runtime_id && request.session.generation == grant.session.generation &&
            request.connection_id.isString() && !grant.connection_id.empty() && request.connection_id.asString() == grant.connection_id &&
            request.task_id.isString() && !grant.task_id.empty() && request.task_id.asString() == grant.task_id &&
            request.action_id.isString() && !grant.action_id.empty() && request.action_id.asString() == grant.action_id &&
            upload_id(grant.upload_id) && request.payload["upload_id"] == grant.upload_id &&
            request.payload["display_id"] == "primary" && request.payload["capture_format"] == "png" &&
            observation.generation == grant.session.generation, "Observation upload scope mismatch");
    const bool capability = destination_.authorization == AuthorizationMode::Host6055UploadId;
    // A dedicated authorization header is supplied by the issuer; it cannot
    // override routing, cookies, framing or the content type.
    const auto& name = grant.credential.header_name;
    if (capability) {
        require(grant.upload_id.size() == 43 && name.empty() && grant.credential.header_value.empty(),
                "Invalid Host observation capability");
        require(observation.sha256_hex.size() == 64 &&
                observation.sha256_hex.find_first_not_of("0123456789abcdef") == std::string::npos,
                "Invalid observation digest");
    } else {
        telemetry::validate_credential(grant.credential);
        require(name == L"Authorization" || name.compare(0, 2, L"X-") == 0,
                "Unsupported observation authorization header");
    }
    require(grant.max_bytes > 0 && grant.max_bytes <= 8u * 1024u * 1024u &&
            grant.max_pixels > 0 && grant.max_pixels <= 16000000 &&
            observation.width > 0 && observation.height > 0 &&
            static_cast<std::uint64_t>(observation.width) * observation.height <= grant.max_pixels &&
            observation.png.size() >= 8 && observation.png.size() <= grant.max_bytes,
            "Observation upload exceeds grant limits");
    const unsigned char signature[] = {137,80,78,71,13,10,26,10};
    require(std::equal(std::begin(signature), std::end(signature), observation.png.begin()),
            "Observation upload is not PNG");
    const auto expiry = capability ? grant.expires_at : std::min(grant.expires_at, grant.credential.expires_at);
    const auto now = std::chrono::system_clock::now();
    require(expiry > now, "Observation upload grant expired");
    require(!capability || expiry <= now + std::chrono::seconds(30), "Host observation capability lifetime exceeded");
    const auto budget = std::min(destination_.timeout,
        std::chrono::duration_cast<std::chrono::milliseconds>(expiry - now));
    require(budget.count() > 0, "Observation upload grant expired");
    const auto deadline = std::chrono::steady_clock::now() + budget;
    require(consumed_.size() < 65536 && consumed_.insert(grant.upload_id).second,
            "Observation upload grant reused or limit reached");
    const auto path = L"/scrp/v1/observations/" + std::wstring(grant.upload_id.begin(), grant.upload_id.end());
    const auto headers = capability ? std::wstring(L"Content-Type: image/png\r\n") :
        name + L": " + grant.credential.header_value + L"\r\nContent-Type: image/png\r\n";
    // Keep asynchronous read storage alive until Request's HANDLE_CLOSING.
    std::array<char, 66> response_buffer{};
    Handle session, connection;
    session.value = WinHttpOpen(L"SandboxRunner/Observation", WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    require(session.value != nullptr, "Cannot open observation HTTPS session");
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    require(WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)),
            "Cannot require observation TLS 1.2");
    const int timeout = static_cast<int>(budget.count());
    require(WinHttpSetTimeouts(session.value, timeout, timeout, timeout, timeout), "Cannot set observation timeouts");
    connection.value = WinHttpConnect(session.value, host_.c_str(), port_, 0);
    require(connection.value != nullptr, "Cannot connect observation HTTPS origin");
    Request http;
    http.attach(WinHttpOpenRequest(connection.value, L"PUT", path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    require(WinHttpSetOption(http.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)),
            "Cannot restrict observation HTTPS request");
    DWORD header_limit = 16384;
    require(WinHttpSetOption(http.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &header_limit, sizeof(header_limit)),
            "Cannot bound observation HTTPS headers");
    started(WinHttpSendRequest(http.value, headers.c_str(), static_cast<DWORD>(headers.size()), nullptr, 0,
                              static_cast<DWORD>(observation.png.size()), reinterpret_cast<DWORD_PTR>(&http)));
    http.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, deadline, expiry, cancelled);
    if (!destination_.trust.leaf_sha256.empty()) {
        PCCERT_CONTEXT cert=nullptr;DWORD length=sizeof(cert);
        require(WinHttpQueryOption(http.value,WINHTTP_OPTION_SERVER_CERT_CONTEXT,&cert,&length),"Cannot inspect observation TLS certificate");
        const auto digest=control::util::sha256_hex(cert->pbCertEncoded,cert->cbCertEncoded);
        CertFreeCertificateContext(cert);
        require(digest==destination_.trust.leaf_sha256,"Observation TLS pin mismatch");
    }
    // Default Schannel chain AND hostname verification stay enabled. No insecure
    // flags, redirect, cookie, automatic authentication or plaintext fallback.
    for (std::size_t offset = 0; offset < observation.png.size();) {
        require(!cancelled && std::chrono::steady_clock::now() < deadline &&
                std::chrono::system_clock::now() < expiry, "Observation upload cancelled or expired");
        const auto count = static_cast<DWORD>(std::min<std::size_t>(65536, observation.png.size() - offset));
        started(WinHttpWriteData(http.value, observation.png.data() + offset, count, nullptr));
        http.wait(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE, deadline, expiry, cancelled);
        require(http.written > 0 && http.written <= count, "Observation HTTPS write incomplete");
        offset += http.written;
    }
    started(WinHttpReceiveResponse(http.value, nullptr));
    http.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, deadline, expiry, cancelled);
    DWORD status = 0, size = sizeof(status);
    require(WinHttpQueryHeaders(http.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
            status == destination_.completed_status, "Observation HTTPS upload not confirmed");
    if (capability) {
        std::string digest;
        for (;;) {
            started(WinHttpReadData(http.value, response_buffer.data(), static_cast<DWORD>(response_buffer.size()), nullptr));
            http.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, deadline, expiry, cancelled);
            require(http.read <= response_buffer.size() && digest.size() + http.read <= 65,
                    "Invalid observation upload receipt");
            if (!http.read) break;
            digest.append(response_buffer.data(), http.read);
        }
        require(digest == observation.sha256_hex + "\n", "Observation upload receipt digest mismatch");
    }
    require(!cancelled && std::chrono::steady_clock::now() < deadline &&
            std::chrono::system_clock::now() < expiry, "Observation upload cancelled or expired");
    return true;
}
} // namespace runner::observation
