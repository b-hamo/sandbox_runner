#include "https_uploader.h"
#include "control/control_util.h"
#include <wincrypt.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>

namespace runner::artifact_transfer {
namespace {
struct Failure { scrp::ArtifactError error; };
void check(bool ok, scrp::ArtifactError error = scrp::ArtifactError::UploadFailed) { if (!ok) throw Failure{error}; }
void check_stop(const std::atomic<bool>& cancelled, std::chrono::steady_clock::time_point deadline,
                std::chrono::system_clock::time_point expiry) {
    check(!cancelled, scrp::ArtifactError::SessionTerminated);
    check(std::chrono::steady_clock::now() < deadline && std::chrono::system_clock::now() < expiry,
          scrp::ArtifactError::UploadExpired);
}
struct Handle {
    HINTERNET value = nullptr;
    ~Handle() { if (value) WinHttpCloseHandle(value); }
};
// Asynchronous operations poll the caller's cancellation flag. Buffers and
// callback state outlive HANDLE_CLOSING on every failure/cancellation path.
struct Request {
    HINTERNET value = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    DWORD completed = 0, error = 0, written = 0, read = 0;
    bool callback_set = false, closing = false, close_requested = false, pin_verified = false;
    std::string pin;
    void close_request() noexcept {
        bool close = false;
        { std::lock_guard<std::mutex> lock(mutex); close = !close_requested; close_requested = true; }
        if (close && value) WinHttpCloseHandle(value);
    }
    static void CALLBACK callback(HINTERNET handle, DWORD_PTR context, DWORD status, void* info, DWORD size) noexcept {
        if (!context) return;
        auto& self = *reinterpret_cast<Request*>(context);
        // As in Microsoft's WinHTTP client certificate-check callback, inspect
        // TLS at SENDING_REQUEST before the request headers leave the client.
        // A failure closes the request within the callback, before any body I/O.
        if (status == WINHTTP_CALLBACK_STATUS_SENDING_REQUEST && !self.pin.empty()) {
            bool valid = false;
            PCCERT_CONTEXT cert = nullptr; DWORD length = sizeof(cert);
            if (WinHttpQueryOption(handle,WINHTTP_OPTION_SERVER_CERT_CONTEXT,&cert,&length) && cert) {
                try { valid = control::util::sha256_hex(cert->pbCertEncoded,cert->cbCertEncoded) == self.pin; } catch (...) {}
                CertFreeCertificateContext(cert);
            }
            {
                std::lock_guard<std::mutex> lock(self.mutex);
                self.pin_verified = valid;
                if (!valid) self.error = ERROR_WINHTTP_SECURE_FAILURE;
                self.changed.notify_all();
            }
            if (!valid) { self.close_request(); return; }
        }
        std::lock_guard<std::mutex> lock(self.mutex);
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
            self.error = size >= sizeof(WINHTTP_ASYNC_RESULT) ?
                static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError : ERROR_WINHTTP_CONNECTION_ERROR;
        if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE && size == sizeof(DWORD)) self.written = *static_cast<DWORD*>(info);
        if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) self.read = size;
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) self.closing = true;
        self.completed |= status; self.changed.notify_all();
    }
    void attach(HINTERNET handle) {
        value = handle; check(value != nullptr);
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        check(WinHttpSetOption(value,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context)));
        check(WinHttpSetStatusCallback(value,callback,WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS,0) != WINHTTP_INVALID_STATUS_CALLBACK);
        callback_set = true;
    }
    ~Request() {
        if (!value) return;
        close_request();
        if (callback_set) {
            std::unique_lock<std::mutex> lock(mutex); changed.wait(lock,[this] { return closing; });
        }
    }
    void wait(DWORD flag, std::chrono::steady_clock::time_point deadline,
              std::chrono::system_clock::time_point expiry, const std::atomic<bool>& cancelled) {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            check_stop(cancelled,deadline,expiry); check(!error);
            if (completed & flag) { completed &= ~flag; return; }
            changed.wait_until(lock,std::min(deadline,std::chrono::steady_clock::now()+std::chrono::milliseconds(20)));
        }
    }
};
void started(BOOL result) { check(result || GetLastError() == ERROR_IO_PENDING); }
bool valid_id(const std::string& s) {
    return s.size() >= 16 && s.size() <= 128 && s.find_first_not_of(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
}
bool valid_token(const std::string& s) {
    return s.size() == 43 && s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos &&
        std::string("AEIMQUYcgkosw048").find(s.back()) != std::string::npos;
}
}
HttpsUploader::HttpsUploader(Destination destination) : destination_(std::move(destination)) {
    const auto& origin = destination_.origin;
    const bool valid = origin.size() <= 2048 && origin.compare(0,8,L"https://") == 0 &&
        origin.find_first_of(L"/?#@\\\r\n\t ",8) == std::wstring::npos && origin.find(L'\0') == std::wstring::npos;
    URL_COMPONENTS url{}; url.dwStructSize = sizeof(url); url.dwHostNameLength = DWORD(-1);
    if (!valid || !WinHttpCrackUrl(origin.c_str(),static_cast<DWORD>(origin.size()),0,&url) ||
        url.nScheme != INTERNET_SCHEME_HTTPS || !url.dwHostNameLength || !url.nPort)
        throw std::invalid_argument("Invalid Artifact HTTPS origin");
    host_.assign(url.lpszHostName,url.dwHostNameLength); port_ = url.nPort;
    if (!destination_.trust.leaf_sha256.empty() && (destination_.trust.leaf_sha256.size() != 64 ||
        destination_.trust.leaf_sha256.find_first_not_of("0123456789abcdef") != std::string::npos))
        throw std::invalid_argument("Invalid Artifact TLS pin");
}
scrp::ArtifactTransferResult HttpsUploader::upload(const scrp::ArtifactUploadRequest& grant,
        artifact::VerifiedFile& source, const std::atomic<bool>& cancelled) noexcept {
    scrp::ArtifactTransferResult result;
    try {
        std::unique_lock<std::mutex> serial(mutex_,std::try_to_lock);
        check(serial.owns_lock(),scrp::ArtifactError::Busy);
        check(!cancelled,scrp::ArtifactError::SessionTerminated);
        check(valid_id(grant.upload_id) && valid_token(grant.upload_token) && scrp::valid_uuid_v4(grant.candidate_event_id) &&
            grant.max_bytes >= 1 && grant.max_bytes <= scrp::artifact_max_bytes,scrp::ArtifactError::Blocked);
        check(static_cast<bool>(source),scrp::ArtifactError::CandidateUnavailable);
        const auto length = source.size();
        check(length <= grant.max_bytes && length <= scrp::artifact_max_bytes,scrp::ArtifactError::Blocked);
        check(source.validate_unchanged() == ERROR_SUCCESS,scrp::ArtifactError::CandidateUnavailable);
        check_stop(cancelled,grant.monotonic_deadline,grant.deadline);
        const auto budget = std::chrono::duration_cast<std::chrono::milliseconds>(grant.monotonic_deadline-std::chrono::steady_clock::now());
        check(budget.count() > 0,scrp::ArtifactError::UploadExpired);
        // Preserve the admission-time monotonic bound. Clock rollback while
        // queued or uploading cannot extend the original total grant budget.
        const auto deadline = grant.monotonic_deadline;
        check(consumed_.size() < 4096 && consumed_.insert(grant.upload_id).second,scrp::ArtifactError::UploadFailed);
        LARGE_INTEGER start{};
        check(SetFilePointerEx(source.handle(),start,nullptr,FILE_BEGIN),scrp::ArtifactError::CandidateUnavailable);
        const auto path = L"/scrp/v1/artifacts/" + std::wstring(grant.upload_id.begin(),grant.upload_id.end());
        const auto headers = L"Authorization: Bearer " + std::wstring(grant.upload_token.begin(),grant.upload_token.end()) +
            L"\r\nContent-Type: application/octet-stream\r\n";
        // Caller owns a fixed size buffer; no whole-file copy or Base64 framing.
        std::array<unsigned char,65536> buffer{};
        std::array<unsigned char,1> response{};
        Handle session, connection;
        session.value = WinHttpOpen(L"SandboxRunner/Artifact",WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
        check(session.value != nullptr);
        // Some MinGW headers expose the option but omit its two-DWORD native
        // structure. Explicitly disable WinHTTP's failed-connection resends.
        struct RetryPolicy { DWORD max_retries; DWORD allowed_conditions; } retries{0,0};
        check(WinHttpSetOption(session.value,WINHTTP_OPTION_FAILED_CONNECTION_RETRIES,&retries,sizeof(retries)));
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        check(WinHttpSetOption(session.value,WINHTTP_OPTION_SECURE_PROTOCOLS,&protocols,sizeof(protocols)));
        const int timeout = static_cast<int>(std::min<std::int64_t>(budget.count(),(std::numeric_limits<int>::max)()));
        check(WinHttpSetTimeouts(session.value,timeout,timeout,timeout,timeout));
        connection.value = WinHttpConnect(session.value,host_.c_str(),port_,0); check(connection.value != nullptr);
        Request http;
        http.pin = destination_.trust.leaf_sha256;
        http.attach(WinHttpOpenRequest(connection.value,L"PUT",path.c_str(),nullptr,
            WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
        DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_KEEP_ALIVE;
        check(WinHttpSetOption(http.value,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled)));
        DWORD header_limit = 16384;
        check(WinHttpSetOption(http.value,WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,&header_limit,sizeof(header_limit)));
        // dwTotalLength emits the exact Content-Length, including a zero byte file.
        started(WinHttpSendRequest(http.value,headers.c_str(),static_cast<DWORD>(headers.size()),nullptr,0,
            static_cast<DWORD>(length),reinterpret_cast<DWORD_PTR>(&http)));
        http.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE,deadline,grant.deadline,cancelled);
        check(http.pin.empty() || http.pin_verified);
        // Schannel's default certificate chain AND hostname verification remain
        // enabled. A pin is additional verification; no insecure flags are set.
        std::uint64_t offset = 0;
        while (offset < length) {
            check_stop(cancelled,deadline,grant.deadline);
            const auto count = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(),length-offset));
            DWORD read = 0;
            check(ReadFile(source.handle(),buffer.data(),count,&read,nullptr) && read == count);
            DWORD written = 0;
            while (written < read) {
                check_stop(cancelled,deadline,grant.deadline);
                started(WinHttpWriteData(http.value,buffer.data()+written,read-written,nullptr));
                http.wait(WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE,deadline,grant.deadline,cancelled);
                check(http.written > 0 && http.written <= read-written);
                written += http.written; result.bytes_sent += http.written;
            }
            offset += read;
        }
        check(source.validate_unchanged() == ERROR_SUCCESS,scrp::ArtifactError::CandidateUnavailable);
        started(WinHttpReceiveResponse(http.value,nullptr));
        http.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE,deadline,grant.deadline,cancelled);
        DWORD status = 0, size = sizeof(status);
        check(WinHttpQueryHeaders(http.value,WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX) && status == 201);
        // One byte suffices to reject every nonempty receipt. Do not log or copy
        // error bodies. 201 is only Host receipt, never safety or final export.
        started(WinHttpReadData(http.value,response.data(),static_cast<DWORD>(response.size()),nullptr));
        http.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE,deadline,grant.deadline,cancelled);
        check(http.read == 0);
        check(source.validate_unchanged() == ERROR_SUCCESS,scrp::ArtifactError::CandidateUnavailable);
        check_stop(cancelled,deadline,grant.deadline);
        result.error = scrp::ArtifactError::None;
    } catch (const Failure& failure) { result.error = failure.error; }
    catch (...) { result.error = scrp::ArtifactError::Internal; }
    return result;
}
} // namespace runner::artifact_transfer
