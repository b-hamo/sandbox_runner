# 실제 Windows Sandbox Artifact 검증 — Issue #23

2026-10-03 16:59~17:01 KST, 실제 Windows Sandbox 두 개에서 동일한 제품 EXE로 전체 시나리오를 각각 통과했다. Host/MCP/Scanner는 아직 이 계약을 구현하지 않았으므로 독립 TLS 테스트 peer를 사용했다. Runner의 실제 Guest 실행·네트워크 전송 검증과 Host 운영 반출 검증을 구분한다.

## 실행 환경과 배치

- Host: Windows 11 Pro `10.0.26200`, Windows Sandbox 앱 `0.8.107.0`.
- Guest: Windows `10.0.26100.0`, `WDAGUtilityAccount`, Microsoft Corporation `Virtual Machine`.
- MSYS2 UCRT64 Release 제품 `sandbox_runner.exe`, 4,329,291 bytes. Guest `--help` 실행과 Host/Guest EXE SHA-256 일치를 확인했다.
- EXE SHA-256: `e47c9cf0adcc28e43008e744fada7ff4f70185c4c4ab0e057d90145f9b93aba5`.
- EXE·bootstrap·공개 인증서·Guest 스크립트만 읽기 전용으로 매핑했다. 개인 키와 수신 검증 증거는 매핑 밖 Host에 뒀다.
- Output은 Guest 내부 `C:\RunnerWorkspace\Sessions\session-<session_id>\runtime-runtime-qa\generation-1\output`이다. Host 쓰기 가능 Output 공유는 없다.
- Guest outbound를 테스트 Host IPv4의 TCP 17443/17444/17445로 제한하고 확인했다. Host 방화벽은 변경하지 않았다.
- Control/Telemetry는 동일 WSS 포트, Artifact는 별도 HTTPS 포트다. Guest의 임시 Root trust, Windows chain/hostname, bootstrap leaf pin을 사용했다. 인증서 검증을 해제하지 않았다.
- [공식 Sandbox CLI](https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-cli)의 `start/connect/exec/stop`으로 정확한 VM ID를 관리했다. Guest 스크립트는 `ExistingLogin`으로 실행했다.

## 실제 결과

| 시나리오 | 두 VM의 결과 |
| --- | --- |
| `결과.txt` 한글 UTF-8 | 안정화 후보 → Telemetry STORED → 요청 → PUT 바이트/크기/SHA-256 일치 → UPLOADED |
| 0바이트 파일 | Content-Length 0, 빈 201 및 bytes_sent 0으로 성공 |
| 사용한 upload_id 재요청 | ARTIFACT_BLOCKED, 추가 PUT 없음 |
| 후보 보고 후 파일 삭제 | CANDIDATE_UNAVAILABLE, PUT 없음 |
| grant 크기 상한 초과 | ARTIFACT_BLOCKED, PUT 없음 |
| 만료된 grant | UPLOAD_EXPIRED, PUT 없음 |
| HTTP 403 | 실제 수신 bytes_sent와 UPLOAD_FAILED 일치 |
| 비어 있지 않은 201 응답 | UPLOAD_FAILED; 수신 성공으로 처리하지 않음 |
| 활성 업로드 중 두 번째 요청 | ARTIFACT_BUSY, 별도 PUT 없음 |
| 278,528-byte PUT 응답 지연 중 TERMINATE | 원 업로드 SESSION_TERMINATED → TERMINATE_RESULT, worker_stopped=true |

지연된 HTTPS 중 별도 heartbeat 응답은 두 번 모두 1ms 미만이었다(0.55ms / 0.64ms). 원 connection·correlation·sequence와 candidate_event_id/upload_id/bytes_sent를 검증했다. HTTP 수신 바이트의 실제 SHA-256을 예상 Guest 바이트와 대조했다.

두 VM 모두 Runner exit 0, Guest 임시 인증서 제거, 잔여 Runner 프로세스 0, peer thread 종료를 확인했다. 마지막 보고는 정상 검증된 TLS 연결을 먼저 연 뒤 Guest trust를 제거하고 같은 연결로 보냈다. 시험 후 해당 VM을 `wsb stop --id`로 정리했고 Sandbox 목록이 비어 있음을 확인했다.

## 발견한 문제와 수정

최초 Guest 실행은 테스트 스크립트의 `ReadAllText` 공유 모드 오류로 중단됐다. 실행 중 로그를 공유 읽기하는 FileStream으로 수정했다. CLI 재기동·PowerShell 5.1 UTF-8 준비 과정의 실패도 별도 run 증거에 보존했으며 통과 횟수에 포함하지 않는다.

수정 전 제품에서는 10개 Artifact 결과와 Runner exit 0을 받았으나 마지막 `TERMINATE_RESULT`가 유실됐다. 기존 WSS 종료는 `WinHttpWebSocketShutdown`의 송신 완료만 기다린 뒤 핸들을 취소했다. 정상 종료를 **전체 1초 기한 안에서 송신 종료 → 기존/신규 receive로 상대 close frame 수신 → WinHttpWebSocketClose 완료**까지 유지하도록 보완했다. 종료 뒤 도착한 메시지는 작업으로 분배하지 않는다. 장애·외부 취소·기한 초과에서는 기존 취소·정리 경로를 유지한다. 이는 transport close이며 Host 업무 처리 ACK가 아니다.

보완 후 일반 Windows 실제 EXE 시험과 새 Sandbox 두 번이 모두 최종 종료 응답까지 통과했다. native 종료 회귀 5개(pending receive 있음/없음, 종료 직전 메시지, close ACK 없음의 1초 제한, 취소)와 기존 HTTPS 16개·WSS TLS/pin 4개도 통과했다. 제품 변경은 `src/transport/telemetry/winhttp_websocket.cpp`에 한정되며 wire/Host 스키마 변경은 없다.

## 증거와 재현

Git 제외 로컬 `build/`에 다음 증거를 보관한다. JSON에는 토큰을 기록하지 않는다.

- `artifact-sandbox-20261003-163958`: 최초 Guest 로그 수집 오류와 인증서·프로세스 정리 보고.
- `artifact-sandbox-20261003-164618`: 클라이언트 재기동 실패/Control handshake timeout.
- `artifact-sandbox-20261003-165329`: 수정 전 제품의 마지막 종료 응답 유실; Artifact 10개 결과와 Guest exit 0은 확인.
- `artifact-sandbox-20261003-165905`, `artifact-sandbox-20261003-170044`: 수정 후 전체 통과. 각 `evidence.json`, `host-cleanup.json`, `guest-exec-result.txt`, stdout/stderr, 읽기 전용 `.wsb` 구성.

기존 팀 방침대로 fixture는 Git 제외 `tests/`의 `artifact_sandbox_host.py`, `artifact_sandbox_guest.ps1`, `run_artifact_sandbox.ps1`에 보관한다. 로컬 fixture가 있는 작업 공간에서 UCRT64 제품을 빌드하고 다음을 실행한다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/run_artifact_sandbox.ps1
```

이 로컬 driver는 테스트 Host IPv4·허용된 Python 실행 경로를 현재 개발 PC에 맞춘다. 다른 PC에서는 해당 설정과 cryptography 준비를 확인해야 한다. 새 checkout만으로 로컬 fixture가 재현되지는 않는다. 세션/토큰/인증서는 매번 새로 발급하며 Sandbox에는 빌드 도구나 Python을 설치하지 않는다.

## 검증하지 않은 범위

실제 Host의 artifact_list/export, 승인 정책, 비공개 저장소와 Runner 결과의 운영 교차 확인, 백신·형식 검사, 최종 EXPORTED/결과 참조 및 실제 Codex MCP 전체 흐름은 후속 Host 통합 대상이다. 이 시험의 HTTP 201/UPLOADED는 파일 안전 판정이나 최종 반출 성공이 아니다.
