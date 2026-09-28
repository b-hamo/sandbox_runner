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
- 실제 Codex CLI가 원본 Host MCP 서버를 stdio로 실행하고 task_submit/runtime_get_state를 호출했다.
  첫 Sandbox 시험(기존 관리 모드)은 WSS 접속 후 gui capability 누락으로 Host가 거부했다.
- 수정된 제품을 배치한 재시험은 Windows Sandbox가 로그인 전
  “서버에 너무 많은 세션이 설정되어 있습니다” 오류를 내어 Runner 접속까지 진행되지 못했다. 실제 Codex는 약 240초간 `PREPARING / connected=false`,
  `computer_observe`는 `RUNTIME_UNAVAILABLE`을 확인하고 `session_stop`으로 종료했다.
  **수정 제품의 실제 Sandbox 화면 반환·클릭·한글 입력·정상 종료는 아직 미검증이다.**
- Host 추적 파일의 전후 SHA-256 목록이 동일함을 확인했다. Host 소스파일은 수정하지 않았다.

로컬 재현 스크립트는 `tests/codex_mcp_sandbox_test.py`, 산출물은
`build/codex-mcp-sandbox-<시각>/`에 있다. bootstrap/token/private key가 포함되므로 Git에 올리지 않는다.
테스트 스크립트는 원본 Host MCP 실행 → bootstrap 생성 → 임시 Sandbox 인증서/읽기 전용 package 배치 →
일반 제품 EXE 실행 → Codex의 관찰/빈 메모장 클릭/한글 입력/재관찰/종료 순서다.
Sandbox 로그인 오류를 해소한 뒤 같은 스크립트를 재실행해야 한다.

## 남은 범위

이 연결은 Output 알림 감시와 GUI를 함께 실행한다. Candidate 생성/Telemetry 전송/Artifact 반출은
별도 계약과 구현 경로이며 여기서 동시에 연결하지 않았다. 네트워크 정책 설정·검증은 외부 Runtime/Host 책임이다.
Control 재연결 및 bootstrap credential 만료 후 갱신은 현재 제품 경로에서 지원하지 않는다.
Host 소스가 바뀌면 정책 revision, lease, 업로드 만료/상한/응답 계약의 호환성을 다시 확인해야 한다.
