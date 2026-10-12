// src/control/control_util.cpp
#include "control_util.h"
#include "protocol/scrp/envelope.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS)(status)) >= 0)
#endif

namespace runner::control::util {

// Reuse the common SCRP identity/time implementation. Preserve this module's
// empty-on-RNG-failure contract for callers and imported regression tests.
std::string utc_now_rfc3339() { return scrp::utc_now(); }
std::string random_uuid_v4() { try { return scrp::uuid_v4(); } catch (...) { return {}; } }
std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32];
    std::string out;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return out;
    }
    bool ok = NT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    // ULONG 한도를 넘는 입력은 나누어 해시한다.
    std::size_t offset = 0;
    while (ok && offset < size) {
        const ULONG chunk = static_cast<ULONG>(
            (size - offset) > 0x10000000u ? 0x10000000u : (size - offset));
        ok = NT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data + offset), chunk, 0));
        offset += chunk;
    }
    if (ok) ok = NT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return out;

    static const char* hex = "0123456789abcdef";
    out.reserve(64);
    for (unsigned char c : digest) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0F]);
    }
    return out;
}

std::optional<std::wstring> utf8_to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return std::nullopt;
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

}  // namespace runner::control::util
