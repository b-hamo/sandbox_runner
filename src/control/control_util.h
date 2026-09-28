// src/control/control_util.h
// Control 계층 내부 유틸리티.
// UUIDv4·UTC 시각은 공용 src/protocol/scrp/envelope.* 구현에 위임한다.
// 해시·엄격한 UTF 변환만 이 모듈에서 구현한다.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace runner::control::util {

// UTC RFC 3339 (밀리초), 예: 2026-09-27T05:00:00.123Z
std::string utc_now_rfc3339();

// BCryptGenRandom 기반 UUIDv4 문자열. 실패 시 빈 문자열.
std::string random_uuid_v4();

// SHA-256 hex(소문자). 실패 시 빈 문자열.
std::string sha256_hex(const std::uint8_t* data, std::size_t size);

// 엄격한 UTF-8 → UTF-16 변환. 잘못된 UTF-8이면 std::nullopt.
std::optional<std::wstring> utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const std::wstring& text);

}  // namespace runner::control::util
