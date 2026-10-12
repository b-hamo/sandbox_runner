# Host observation 연결 (#17)

Issue #23은 기존 관찰/GUI 계약을 유지하면서 bootstrap의 명시적 `artifact-export-v1` 선택으로
별도 후보·Telemetry·Artifact HTTPS 경로를 추가한다. [설정과 Host 추가 구현](artifact-export-contract.md)을 따른다.
아래 실제 Sandbox 검증은 기존 GUI/관찰에 대한 이력이며 새 Artifact 전체 반출의 검증을 뜻하지 않는다.

## 적용 기준

Host 저장소 `b-hamo/host_control`, `feat/19-observation-upload`의 `6055cc6`을 수정하지 않고 연결한다.
이 문서의 상수는 해당 Host 구현에 대한 호환 프로필이며 SCRP 전체의 동결 규격이 아니다.
Issue #21에서 Host PR #23/#25 (`b08a74d`)가 추가한 bootstrap 인증서 SHA-256 필드를
하위 호환으로 지원한다. 이것은 bootstrap 입력 호환성 수정이며 해당 Host/Manager의
전체 기동·TLS·GUI·종료 흐름에 대한 실환경 통합 검증 완료를 뜻하지 않는다.
2026-09-28 사용자 확인: `monitoring_coverage.file=true`는 **세션 Output 폴더 감시**를 뜻한다.
전체 Sandbox 파일 활동 감시나 보안 검사 완료를 뜻하지 않는다.

## 실행과 책임

```powershell
.\sandbox_runner.exe --host-bootstrap C:\RunnerPackage\bootstrap.json --host-address 192.168.0.3
```

- 원본 Host bootstrap v1.0을 읽는다. JSON은 64 KiB로 제한하고 필드/버전/ID/포트/만료를 검증한다.
- 필수 최상위 필드는 `bootstrap_version`, `session_id`, `runtime_id`, `generation`,
  `host`, `port`, `path`, `token`, `token_expires_at`, `host_certificate_pem`, `observation_upload`다.
  `host_certificate_sha256`만 선택 필드로 추가 허용한다. 미정의 필드와 필수 필드 누락은 거부한다.
- 선택 해시는 인증서 **DER 바이트**의 SHA-256을 표현한 소문자 16진수 64자리 문자열이다.
  PEM 문자열 자체의 해시가 아니다. null·다른 타입·대문자·공백·잘못된 길이는 거부하며,
  PEM에서 직접 계산한 해시와 다르면 접속 전에 `Bootstrap certificate SHA-256 mismatch`로 거부한다.
  필드가 없으면 이전처럼 PEM에서 pin을 계산한다. 제공된 해시로 계산된 pin을 덮어쓰지 않는다.
- Host MCP의 `host:null`에는 launcher가 실제 Host IPv4를 명시한다. 임의 gateway 추정은 하지 않는다.
  bootstrap이 주소를 지정하면 CLI 주소와 일치해야 한다. 현재 DNS/IPv6 주소 입력은 지원하지 않는다.
- launcher는 신뢰 앵커와 주소 SAN이 유효한 인증서를 준비해야 한다. Runner가 인증서를 설치하거나
  TLS 검증을 해제하지 않는다. Windows chain/hostname 검증에 bootstrap leaf SHA-256 pin을 추가한다.
- EXE/bootstrap은 읽기 전용 매핑으로 전달할 수 있다. Output은 Guest의
  `C:\RunnerWorkspace\Sessions` 아래 session/runtime/generation별 폴더다. Host 쓰기 공유는 없다.
- Output watcher의 실제 ready 이후 `file=true`, `gui.observe`, `gui.input`을 HELLO에 광고한다.
  감시 시작 실패는 연결 전에 거부한다. 감시 오류는 세션을 멈추고 입력을 차단한다.
- Runner가 WSS로 먼저 접속한다. 인증된 HELLO_ACK에서 capability를 교집합으로 제한하고 Worker를 시작한다.
  `Host6055` 프로필에서는 **Host Broker가 시작 검증 및 입력 요청 발행을 책임진다**.
  존재하지 않는 시작 승인 메시지나 network/startup verified 값을 만들어 보내지 않는다.
- Host의 STATE → OBSERVE → HEARTBEAT 검사에 응답하고 Host가 READY 이후 보내는 Action을 수행한다.
  현재 Host 정책 `POL-0.1.0`과 초기 15초 lease를 적용하고 HEARTBEAT로 갱신한다.
  기존 VerifiedGrant 내부 주입 경로의 사전 검증 조건은 유지한다.
- 세션·세대·connection·task·action·관찰 좌표 바인딩, 연결 단절/lease 만료/종료 차단을 유지한다.
  Output 건강 상태가 회복되어도 이미 차단된 세션을 자동 재활성화하지 않는다.
- 캡처는 Host OBSERVE 요청에만 수행한다. 주기적인 화면 전송이나 시작 시 임의 캡처는 없다.

## Issue #21 bootstrap 검증 (2026-09-30)

실제 제품 EXE의 입력 경로로 기존 11개/신규 12개 필드, PEM LF/CRLF,
잘못된 해시 타입·형식·값, PEM 텍스트 해시, 미정의 필드, 모든 필수 필드의
누락 및 미정의 필드로 교체를 검사한 로컬 테스트 63개를 통과했다.
정상 입력은 인증서 검사 다음의 만료 자격 검사까지 도달하는지 확인한다.
테스트 자격은 의도적으로 만료시켜 실제 네트워크/GUI 기동을 하지 않는다.
테스트는 기존 방침대로 로컬 `tests/bootstrap_sha256_test.py`에 보관한다.
MSYS2 UCRT64 Release 빌드 및 기존 CTest 12개도 통과했다.
새 Host PR #25와 Runtime Manager의 실제 Sandbox 전체 통합 시험은 별도 수행해야 한다.

## PNG 수신 계약

Host `ObservationUploads`가 발급한 43자 upload ID 자체가 제한된 일회성 권한이다.
인증된 Control OBSERVE에서 받은 ID만 사용하며 다른 연결의 토큰을 업로드에 재사용하지 않는다.
`https://<bootstrap Host>:<observation_upload.port>/scrp/v1/observations/<upload_id>`에
Content-Type image/png와 Content-Length로 PNG를 PUT한다. 이 프로필에는 별도 Authorization 헤더가 없다.
Runner는 요청 binding, ID, PNG 크기 8 MiB 이하, 픽셀 16,000,000 이하, 최대 30초 만료를 확인한다.
PUT의 전체 기한은 5초 또는 권한 잔여 시간 중 짧은 값이다.
HTTP **201과 본문의 정확한 SHA-256 + LF**를 확인한 다음 OBSERVE_RESULT 메타데이터를 돌려준다.
Control JSON에는 PNG/Base64를 넣지 않는다. Host MCP가 수신·검증한 PNG를 Codex의 image content로 반환한다.

201은 최종 Host 검증과 별개다. Host는 OBSERVE_RESULT의 sha256/크기와 수신 PNG를 대조한다.
현재 Host 검증은 PNG 구조/CRC/IHDR/IEND 검사이며 모든 픽셀의 완전한 디코딩 검사는 아니다.
재전송·redirect·인증서 검증 해제·평문 fallback은 하지 않는다.

## 검증 결과와 제한

- Windows UCRT64 Release 제품 빌드 및 CTest 12/12 통과. PE 의존성은 Windows 기본 DLL뿐이다.
- bootstrap의 추가 필드, 주소 충돌, 0 generation/port, 잘못된 인증서/만료 시각 6개 거부 검증 통과.
- 기존 HTTPS 전송 12개 회귀 시나리오도 통과했다(상태/redirect/단절/취소/timeout/만료/TLS).
- 추가 runtime 검증: Output 감시 미준비 거부, Host 프로필에서 허위 검증 bool 없이 활성화,
  감시 실패 차단, 복구 후 자동 재활성화 거부. Host 데스크톱 캡처·입력은 수행하지 않았다.
- 원본 Host HTTPS 수신기와 실제 C++ WinHTTP 업로더 5개 시나리오 통과:
  정상 수신/VALIDATED·원본 바이트 일치, 사용한 ID 재사용 거부, 해시 불일치 거부/BLOCKED,
  알 수 없는 ID 거부, 만료 ID 거부. 테스트 임시 Root 인증서는 finally에서 제거했다.
- 실제 Codex CLI가 원본 Host MCP 서버를 stdio로 실행하고 제품 `sandbox_runner.exe`를 실제 Windows Sandbox에서 시험했다.
- 2026-09-28 15:49 최종 시험: **Codex → MCP → Host WSS → Runner → PNG HTTPS 수신 → Codex 이미지 확인 → 클릭 → 한글 입력 → 재관찰 → 정상 종료를 통과했다.**
- Host는 14.7초 후 GUI capability, file coverage, worker, 첫 PNG, Heartbeat를 확인해 READY로 전환했다.
  2030×1230 PNG를 VALIDATED로 수신했고 Codex가 반환 이미지를 보고 빈 편집 영역을 클릭했다.
- `computer_click`: SUCCESS / input_delivered=true. `computer_type`: SUCCESS / chars_sent=5.
  후속 이미지에서 `안녕하세요`를 확인했고, Guest 테스트 편집기가 기록한 UTF-16 문자열도 정확히 일치했다.
  입력 전후 PNG 해시가 달라졌으며 최종 PNG는 1,267,599 bytes였다.
- `session_stop`: TERMINATE_RESULT 및 stopped=true. Guest에서 제품 종료 코드 0과 프로세스 종료를 별도 확인했다.
- Host 추적 파일의 전후 SHA-256 목록이 동일함을 확인했다. Host 소스파일은 수정하지 않았다.
  재시험에서 Runner 제품 소스도 수정하지 않았다(검증 제품 커밋 `df28a54`).

### 이전 실패 원인과 재현 산출물

이전에는 Windows Sandbox 로그인 전 “서버에 너무 많은 세션이 설정되어 있습니다” 오류로
연결 검증이 막혔다. 이번 재시험에서는 새 Sandbox가 정상 로그인되어 그 오류는 재현되지 않았다.
이전 로그인 오류의 OS 내부 근본 원인은 확정하지 않았다.

추가로 테스트 시작 스크립트가 `Start-Process notepad.exe`에서 중단되는 문제를 확인했다.
이 Sandbox에는 Notepad가 없으며, 해당 명령이 Runner 실행보다 앞에 있어 연결이 시작되지 않았다.
Host/Runner 통신 오류가 아니었다. 로컬 테스트 스크립트만 수정하여 간단한 Windows EDIT 컨트롤의
`Runner Input QA` 앱을 입력 대상으로 사용했다. 이 앱은 명령을 보내거나 Runner를 대신하지 않으며,
클릭·텍스트 입력은 원본 Host MCP를 통해 실제 제품 Runner가 수행했다.
이후 새 Sandbox에서 추가 수동 기동 없이 전체 시험을 통과했다.

로컬 스크립트는 `tests/codex_mcp_sandbox_test.py`, 최종 성공 산출물은
`build/codex-mcp-sandbox-20260928-154850/`에 있다. `host.log`, `codex.jsonl`, `codex-result.txt`,
`verification.json`, Host 파일 전후 해시 목록으로 결과를 확인한다.
bootstrap/token/private key도 포함되므로 이 폴더는 Git에 올리지 않는다.
Codex JSONL은 이미지 바이트를 보존하지 않으므로 이미지 보관 파일이 있다고 주장하지 않는다.
실제 이미지 관찰은 Codex 최종 보고와 독립적인 Sandbox 화면/Guest 입력 문자열 확인으로 검증했다.
테스트 앱·스크립트는 로컬 전용이며 제품 배포물이나 Git 추적 파일에 포함하지 않는다.

## 남은 범위

이 연결은 Output 알림 감시와 GUI를 함께 실행한다. Candidate 생성/Telemetry 전송/Artifact 반출은
별도 계약과 구현 경로이며 여기서 동시에 연결하지 않았다. 네트워크 정책 설정·검증은 외부 Runtime/Host 책임이다.
Control 재연결 및 bootstrap credential 만료 후 갱신은 현재 제품 경로에서 지원하지 않는다.
bootstrap 만료는 새 연결의 인증 기한이다. 유효한 HELLO_ACK로 인증된 기존 Control
연결은 그 시각을 넘어 유지하며, heartbeat lease·연결 단절·TERMINATE 규칙을 따른다.
Host 소스가 바뀌면 정책 revision, lease, 업로드 만료/상한/응답 계약의 호환성을 다시 확인해야 한다.
