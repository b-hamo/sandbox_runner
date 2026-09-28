# Host observation 연결 (#17)

## 적용 기준

Host 저장소 `b-hamo/host_control`, `feat/19-observation-upload`의 `6055cc6`을 수정하지 않고 연결한다.
이 문서의 상수는 해당 Host 구현에 대한 호환 프로필이며 SCRP 전체의 동결 규격이 아니다.
2026-09-28 사용자 확인: `monitoring_coverage.file=true`는 **세션 Output 폴더 감시**를 뜻한다.
전체 Sandbox 파일 활동 감시나 보안 검사 완료를 뜻하지 않는다.

## 실행과 책임

```powershell
.\sandbox_runner.exe --host-bootstrap C:\RunnerPackage\bootstrap.json --host-address 192.168.0.3
```

- 원본 Host bootstrap v1.0을 읽는다. JSON은 64 KiB로 제한하고 필드/버전/ID/포트/만료를 검증한다.
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
Host 소스가 바뀌면 정책 revision, lease, 업로드 만료/상한/응답 계약의 호환성을 다시 확인해야 한다.
