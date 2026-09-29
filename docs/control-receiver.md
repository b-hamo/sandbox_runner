# Control 요청 리시버

Issue [#13](https://github.com/b-hamo/sandbox_runner/issues/13)의 수신 기반 구현이다.
SCRP W1 v0.1 문서 5~9쪽의 연결 방향·Envelope·허용 요청 타입을 따른다.
공용 Receiver에는 handshake/payload 계약을 주입한다. 제품 Control 모드에는
Host `feat/1-host-sender`의 `fed305b06dfcfdced406c316514cd3fd2e03a911` 스키마를 따른
명시적 `host-sender-fed305b` adapter를 추가했다. Host 문서의 확정 대기 항목은 동결로 간주하지 않는다.

## 현재 구현

`src/transport/control/control_receiver.*`의 `control::Receiver`는 다음 순서로 동작한다.

```text
외부 Context + Schema + 타입별 Handler
  → run(stop) → Host Control WSS 연결 (Windows TLS 검증)
  → HELLO → HELLO_ACK 검증 → RECEIVING
  → 요청 Envelope 검증 → 외부 payload 검증 → 등록된 Handler
  → 응답 스키마·크기 검증 → 요청 correlation/task/action 및 송신 sequence를 붙여 송신
  → 취소 또는 실패 → socket close → STOPPED 또는 FAILED
```

`RECEIVING`은 handshake 이후 수신 가능 상태다. Host가 확정하는 Runtime READY가 아니다.
수신 대상은 `OBSERVE`, `ACTION_REQUEST`, `STATE_REQUEST`, `HEARTBEAT`,
`ARTIFACT_REQUEST`, `TERMINATE`다. 해당 타입의 핸들러가 등록되어 있어야 전달한다.
`ERROR`를 포함한 다른 타입은 현재 처리하지 않고 연결 실패로 노출한다.
제품 관리 핸들러는 `TERMINATE_RESULT` 송신 이후 수신 루프를 끝내고 프로세스를 종료한다.

공통 Envelope 파서는 버전, 필드 목록, JSON 형태·깊이·크기, UUID/nonce, UTF-8을 검증한다.
Receiver는 현재 session/runtime/generation, connection, 정확히 다음 sequence,
timestamp 허용 오차, 연결 내 message_id·nonce 중복을 검증한다.
요청의 correlation_id/status/error는 null이어야 한다.
ACTION_REQUEST와 OBSERVE는 task_id/action_id가 필요하며 같은 action_id를 다시 전달하지 않는다.
동일 ID의 다른 payload도 거부한다. 실행 기록·상태 조회 및 연결 간 중복 방지는 후속 계층의 책임이다.

잘못된 메시지·스키마·미등록 핸들러·핸들러 예외·연결 끊김·인증 완료 전 자격 만료는 FAILED로 끝난다.
`snapshot().diagnostic`은 고정 진단만 제공하고 peer payload나 자격을 로그에 노출하지 않는다.
`ReplyHandlers`는 최대 2개 응답(ACK + 결과)을 반환할 수 있다. 전체 batch를 검증한 후 송신한다.
제품은 ALIVE, STATE_RESULT, TERMINATE_RESULT 및 미지원 요청에 대한 ERROR를 보낸다.
dispatch 횟수는 핸들러 및 응답 송신 완료 횟수이며,
Host ACK, 실행 완료, 파일 안전성 판정이 아니다.

## 내부 연결 인터페이스

- `Context`: 기존 `scrp::SessionContext`, `telemetry::ConnectionSettings`, credential provider,
  `shared_ptr<const Schema>`를 받는다. Control 전용 endpoint·자격을 주입하고 Telemetry 자격을 재사용하지 않는다.
- `Schema::hello`: 실제 지원 Capability/Coverage 등을 담은 합의된 HELLO payload 생성.
- `Schema::hello_ack`: 선택 버전·허용 범위·제한·채널 자격 등 닫힌 ACK 계약을 검증하고 connection ID 반환.
- `Schema::validate_request`: 타입별 닫힌 payload, operation 및 허용 범위 검증. 실패 시 예외.
- `Schema::validate_reply` / `message_limit`: 응답 계약과 검증된 HELLO_ACK의 메시지 한도.
- `Handlers`: 허용된 요청 타입별 콜백. 기본 핸들러·임의 RPC·GUI 실행 함수는 없다.
- `ReplyHandlers`: 응답 목록과 `finish`를 반환하는 동기 콜백. finish는 송신 후 종료한다.
  Receiver가 connection, 새 message_id/nonce, correlation_id, task/action ID, 송신 sequence를 관리한다.
  등록되지 않은 응답 계약은 기본적으로 송신을 거부한다. Issue #14의 Hooks::poll이 검증된 지연 응답을 전달한다.
- `run(const atomic<bool>& stop)`: 호출 스레드에서 실행하는 단일 사용 blocking 수신 루프.
  소유자가 worker를 만든 경우 stop을 설정하고 join한 뒤 Receiver 및 콜백 참조 대상을 해제한다.
- `snapshot()`: 다른 스레드에서 상태·분배 횟수·진단 조회 가능.

스키마와 핸들러는 빠르게 반환해야 한다. 핸들러는 용량이 제한된 작업 큐로 인계하며
GUI 동작·업로드·긴 I/O를 수신 스레드에서 실행하지 않는다. 큐가 가득 찬 경우 예외로 인계 실패를 알린다.
Receiver는 작업 큐를 소유하지 않는다. GUI 연결 시 disconnected hook이 Runtime을 block하고 owner가 join한다.
후속 소유 계층은 run 종료/실패 시 미시작 작업을 차단하고 큐를 정리해야 한다.
이미 진행 중인 콜백을 강제 중단할 수 없으므로 종료 시간은 콜백의 신속한 반환에 의존한다.

WSS 어댑터는 기존 `telemetry::WebSocket`/WinHTTP 구현을 별도 인스턴스로 재사용한다.
기존 파일의 위치와 namespace는 유지하고 TLS·프레임 처리 코드를 복제하지 않는다.
테스트는 `SocketFactory`로 transport를 대체할 수 있다.

기본 HELLO 대기는 5초이며 Options로 최대 60초까지 조정할 수 있다.
공통 transport의 전체 메시지 상한 기본값은 64 KiB다.
replay 기록은 연결당 기본 65,536개 메시지(HELLO_ACK 포함)까지이며 초과 입력은 연결을 종료한다.
action_id는 기록 메모리 제한을 위해 256바이트까지 허용한다. 이는 구현의 로컬 한도이며 동결된 wire 스키마가 아니다.
수신 취소는 20ms poll과 기존 WinHTTP 취소 경로를 사용한다. 자동 재연결·요청 재전달은 없다.
특히 bootstrap token을 자동 재사용하지 않는다. 자격 만료는 접속 전·직후와
HELLO_ACK 검증 완료 전까지 검사한다. 인증 완료 후에는 bootstrap token 만료만으로
기존 연결을 종료하지 않는다. Runtime의 heartbeat lease와 연결/세션 검증은 계속 적용한다.

### Issue #19 검증 (2026-09-29)

MSYS2 UCRT64 GCC 16.2.0, CMake/Ninja Release 제품 빌드와 로컬 CTest 12개를 통과했다.
Control 수신부는 39개 시나리오를 통과했다. 추가한 6개는 접속 전 만료, 접속 중 만료,
HELLO_ACK 수신 중 만료, 인증 후 수신 중 만료, 인증 후 idle 중 만료,
인증 후 만료 상태에서도 잘못된 connection ID 거부를 확인한다.
동일 테스트가 수정 전 develop 코드에서 실패하고 수정 후 통과함을 확인했다.
기존 gui_runtime/gui_session 테스트의 lease 만료 및 연결 단절 시 입력 차단도 통과했다.
테스트 소스는 저장소 기존 방침대로 Git에서 제외된 로컬 `tests/`에 보관한다.

2026-09-29 실제 Windows Sandbox에 수정 EXE를 배포하고 Host `6055cc6`의 MCP stdio
`tools/call`로 추가 검증했다. 기존 데스크톱 MCP의 만료된 세션과 분리한 새 테스트 세션이다.
14:40:39 KST에 발급한 bootstrap은 14:45:39에 만료됐으며, 14:46:39에도
`runtime_get_state`가 Runtime의 `READY / health=OK / connected=true`를 반환했다.
같은 연결에서 만료 1분 후 `computer_observe`도 `image_state=VALIDATED`로 PNG를 반환했다.
실제 5분 만료를 넘긴 연결 유지·상태 조회·화면 업로드는 통과했다.

후속 입력 검사에 사용하려던 `Win+R`은 Host의 `P-DENY-HOTKEY` 정책으로 거부됐다.
이 예상하지 못한 정책 응답에서 테스트 클라이언트가 종료됐으므로 문자 입력과
`session_stop`/TERMINATE 정상 종료 검증은 완료하지 않았다. 정책을 우회하거나 해제하지 않았다.
배포 EXE SHA-256: `6BC6A7C26AF6AEA3FFFF2C326A657EADB10BFE279D688CFC1AF37C0AE2A036EF`.

## 제품 실행과 후속 범위

제품 CMake가 `runner_control` STATIC 라이브러리를 빌드한다.
`main.cpp`의 `--control-context <파일>`로 Control 관리 모드를 명시적으로 선택한다.
`control_session.*`가 worker·stop/join을 소유한다. 기존 `--session-context` Artifact/Telemetry 모드와
동시에 선택할 수 없으며 Control 모드에서는 Output 감시와 Telemetry를 시작하지 않는다.
GUI Capability는 비어 있고 Monitoring Coverage는 모두 false다. 생존·상태 응답은
DEGRADED/worker_alive=false로 미구현 상태를 드러낸다. 조회한 Action은 UNKNOWN이다.
실행 예제와 실제 Host 테스트 구분은 [Host 연동 문서](host-sender-integration.md)를 따른다.

다음 사항을 합의·구현한 뒤 main 시작·종료 흐름에 연결한다.

1. Host 초안 중 확정 대기 항목의 팀 합의, 실제 Worker 구현에 따른 Capability·허용 범위.
2. ACK의 Telemetry 자격을 기존 Session Context로 전달하고 Control/Artifact 동시 lifecycle 연결.
3. #14에서 비동기 ACK/RESULT, Runtime lease/상태/차단 연결 구현. 실제 Host READY/업로드 계약 주입은 후속.
4. #14에서 GUI 큐·Action 기록·연결 종료 차단 구현. 자동 재연결·재개는 제공하지 않음.
5. ARTIFACT_REQUEST의 현재 후보·경로 재검증 및 제한된 HTTPS 업로드.

W1 PDF의 candidate_id·Guest 해시 등 과거 Artifact 표현은 Issue #11 계약을 덮어쓰지 않는다.
Receiver는 Artifact payload를 임의 확정하거나 파일을 열지 않는다.

## 검증

2026-09-28 Host Windows / MSYS2 UCRT64 GCC 16.2.0에서 제품 Release 빌드와
기존 Artifact/Telemetry/Lifecycle을 포함한 CTest 7개가 통과했다.
Receiver 단위 테스트 33케이스는 정상 6종 분배, handshake 이전 요청, 바인딩·sequence·시각,
미지원 타입·스키마·미등록 핸들러, 중복 ID/nonce/action, 크기·기록 제한,
timeout·취소·peer 종료·핸들러 예외·자격 만료·단일 실행을 확인한다.
테스트 코드는 기존 저장소 방침대로 Git 제외 `tests/`에 보관한다.
실제 WinHTTP loopback WSS 테스트 10개도 통과했다. 정상 수신(분할 프레임), 수신 취소,
미신뢰 인증서·호스트명·pin 불일치, HELLO timeout, 잘못된 세션·payload,
binary frame·64 KiB 초과 메시지 거부를 확인했고 임시 인증서를 제거했다.

```powershell
# MSYS2 UCRT64 도구가 PATH에 있는 Host에서
cmake -S . -B build/issue13/product -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/issue13/product
cmake -S tests -B build/issue13/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OBJECT_PATH_MAX=190
cmake --build build/issue13/tests
ctest --test-dir build/issue13/tests --output-on-failure
python tests/control_wss_test.py build/issue13/tests/control_wss_probe.exe
```

WSS 테스트는 Python cryptography가 필요하며 loopback 테스트 인증서를 현재 사용자 Root 저장소에
일시 등록한 뒤 finally에서 정확한 해당 인증서를 제거한다. 실제 Host 자격은 사용하지 않는다.
한글 경로가 object 출력 경로에 포함될 때의 GCC assembler 오류는 로컬 테스트 CMake의
`CMAKE_OBJECT_PATH_MAX=190` 설정으로 경로를 해시해 해결했다.
일반 PowerShell에서 제품 `--help` 실행과 Windows 기본 DLL만 사용하는 것도 확인했다.
후속 Host 연동 검증에서는 기존 테스트 7개에 Host 스키마와 응답 경계 테스트를 추가했다.
실제 Host Sender의 테스트 결과와 제한은 [Host 연동 문서](host-sender-integration.md)에 기록한다.
이번 Control 모듈의 Windows Sandbox 실행은 미검증이다.

## GUI 지연 응답 연결 (#14)

기본 CLI 관리 모드는 유지한다. 신뢰된 Host adapter가 GuiSession을 주입하면 Replies::deferred로 원 요청을 보존하고 Hooks::poll에서 최종 응답을 보낸다. ACK를 먼저 보내고 이후 결과를 송신하며, 최대 33개 작업과 종료 1개를 추적한다. Hooks::connected는 검증된 HELLO_ACK 후 호출되고 disconnected는 모든 종료 경로에서 실행을 차단한다. 구체 계약과 미구현 운영 연결은 [GUI 연동 문서](gui-integration.md)를 따른다.
