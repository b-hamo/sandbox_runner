// src/control/screen_capture.h
// 주 화면 1개를 원본 픽셀 크기 그대로 PNG로 캡처한다. (SCRP v0.1 6쪽: 초기에는 주 화면 하나, PNG)
// 좌표계는 좌상단 (0,0)이며, 프로세스가 Per-Monitor DPI Aware일 때 캡처 픽셀 = 입력 좌표다.
// 업로드(HTTPS PUT)는 이 모듈의 책임이 아니다. 결과 바이트는 호출자에게 반환만 한다.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace runner::control {

struct CaptureLimits {
    std::size_t max_png_bytes = 8u * 1024u * 1024u;       // 8 MiB (v0.1 제안 기본값)
    std::uint64_t max_pixels = 16000000ull;  // 16 메가픽셀
};

struct CaptureResult {
    bool ok = false;
    std::string error;       // 실패 사유(진단용)
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> png;
};

// 프로세스 DPI 인식 수준을 Per-Monitor V2로 설정한다. 창 생성 전, 프로세스 시작 직후 1회 호출.
// 반환값: 설정 성공 또는 이미 DPI Aware 상태이면 true.
bool enable_per_monitor_dpi_awareness();

// 현재 주 화면의 픽셀 크기.
void primary_screen_size(int& width, int& height);

CaptureResult capture_primary_display_png(const CaptureLimits& limits = {});

}  // namespace runner::control
