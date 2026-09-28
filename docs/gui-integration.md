# GUI 실행 모듈 연동 (#14)

## 구현 상태와 경계

제공된 `sandbox_runtime_cpp/sandbox_runtime/src/control` 및 `src/runtime`의 C++ 실행
모듈을 기존 Runner에 통합했다. 별도 제품 EXE나 상주 프로세스는 추가하지 않는다.
`runner_gui` 정적 라이브러리는 `runner_lifecycle`을 통해 `sandbox_runner.exe`에 연결된다.
`SandboxRuntime`은 Guest 실행 권한·작업 디렉터리·Worker를 관리하며 Host의 VM 생성/폐기
Runtime Manager를 대체하지 않는다.

**현재 CLI의 `--control-context`는 관리 모드다. 이것만 실행해 실제 GUI 입력이 활성화되지는 않는다.**
`run_control_session(context, stop_event, &gui)`라는 내부 연결점을 구현했다.
제품 Host adapter가 `GuiSession`에 실제 검증된 시작 승인과 제한된 HTTPS uploader를
주입해야 GUI가 활성화된다. 둘 중 하나라도 없으면 GuiSession 생성은 실패한다.
기존 Artifact/Telemetry CLI와 동시 기동은 이번 변경에 포함하지 않는다.

Host 기준은 #13과 동일한 `b-hamo/host_control` commit `fed305b06dfcfdced406c316514cd3fd2e03a911`이다.
그 스키마에는 Runtime 모듈이 요구하는 시작 검증 grant가 없고 OBSERVE에는 upload_id만 있다.
업로드의 목적지·일회성 자격·만료·상한을 추측하거나 Control Bearer를 재사용하지 않았다.
제공 문서의 Host READY 요구 조건을 무조건 true로 채우는 우회도 없다.

## 연결 순서

1. Host가 session/runtime/generation 및 Guest 내부의 신뢰된 고정 workspace base를 준비한다.
   base는 기존 로컬 디렉터리여야 하며 Host의 쓰기 가능한 공유 경로를 사용하지 않는다.
2. `GuiSession(config, authorize, upload)`을 만든다. 기본 WorkerBackend는 Windows GDI/WIC/SendInput이다.
  단위 테스트에서만 C++의 fake backend를 주입했다. 실제 Sandbox 검증은 기본 backend를 사용한다.
  wire/JSON으로 backend를 선택할 수 없다.
3. `gui.startup_probe()`의 PNG를 별도 시작 검증 경로로 Host가 검증한다. probe는 좌표 입력용
   observation_id를 만들지 않는다. 로컬 캡처 성공만으로 Host 검증 플래그를 설정하지 않는다.
4. Control Context의 session을 동일하게 연결하여 `run_control_session(context, stop_event, &gui)`을 호출한다.
   함수가 GUI schema/handlers/hooks를 Receiver에 연결한다.
5. WSS 인증·HELLO_ACK 검증 후 `Authorize`가 호출된다. 빠르게 반환하는 내부 콜백이어야 하며
   수신 스레드에서 네트워크나 긴 검증 작업을 실행하지 않는다. 사전 확인된 HostGrant를 반환하거나 거부한다.
   Runtime이 session/runtime/generation/connection/policy, startup/network/monitoring 검증과
   1~15000ms lease를 확인한다. grant capability와 HELLO_ACK capability의 교집합만 허용한다.
6. Worker 결과는 크기 제한 큐로 보내며 Receiver만 WSS를 송신한다. 종료 시 GUI 차단 → Worker/uploader
   종료 대기 → TERMINATE_RESULT → 소켓/수신 worker 정리 순서다. 외부 stop/연결 실패에도 차단 후 join한다.

호출자가 GuiSession을 Receiver보다 오래 유지해야 한다. callback은 GuiSession을 파괴하거나 stop/join하면 안 된다.
`GuiSession::workspace().output`이 Artifact 감시 연결점이다. 이 경로에 기존 CandidateDetector를
연결하고 해당 세션의 Telemetry를 함께 시작하는 lifecycle은 별도 작업이다.

## 요청·결과 매핑

| Host 요청 | 실행 및 응답 |
| --- | --- |
| OBSERVE primary/png | 직렬 캡처 → 전용 uploader → 업로드 완료 후 OBSERVE_RESULT 메타데이터 |
| mouse.move/click/scroll | observation_id·좌표/해상도·10초 만료 확인 후 SendInput |
| keyboard.type | 엄격한 UTF-8, 최대 4096 bytes, Unicode/개행/Tab 입력 |
| keyboard.press | 모듈이 지원하는 고정 Key enum만 허용 |
| keyboard.hotkey | Ctrl/Shift/Alt modifier들 뒤에 주 키 한 개. 다른 순서/미지원 키는 거부 |
| ui.click_element, Win 키 등 | wire 검증 후 ACK REJECTED, 입력 없음 |
| HEARTBEAT | UTC 만료를 남은 시간으로 변환하여 최대 15초 monotonic lease 갱신, ALIVE |
| STATE_REQUEST | 실제 ledger의 PENDING/RUNNING/최종 상태, 없는 ID는 UNKNOWN |
| TERMINATE | 비동기 stop/join, 완료된 경우에만 worker_stopped=true |
| ARTIFACT_REQUEST | 기존처럼 미지원 ERROR, Artifact 전송/안전 판정 없음 |

ACK ACCEPTED는 접수만 의미한다. 빠른 Worker가 먼저 완료해도 ACK를 먼저 송신한 뒤 poll에서
ACTION_RESULT를 보낸다. Receiver가 원 요청을 보존하여 correlation/task/action/session을 붙인다.
송신 sequence는 ACK·관리 응답·비동기 결과 전체에서 단조 증가한다.
Receiver의 기존 중복 action_id 거부 정책을 유지하며 재실행·자동 재연결하지 않는다.

입력 SUCCESS는 Windows 입력 전달 성공이며 사용자 목표의 성공 판정이 아니다.
`chars_sent`는 제공 모듈의 UTF-16 code unit 진행량이다(Unicode code point 개수와 다름).
ERROR의 code는 현재 adapter가 검증하는 Host enum `UNSUPPORTED_TYPE`을 유지한다.
캡처/업로드 실패·취소의 구체 상황은 고정 message로 구분하며, 정교한 오류 코드 매핑은 후속 계약 사항이다.

## 제한·실패·수명

- 대기 입력은 내부 최대 32개, Host 협상 max_queue_depth도 접수 시 적용한다.
- 완료 대기 요청은 최대 33개, 종료용 1개를 추가로 보존한다. 송신 큐는 최대 34개다.
  이 한도는 HEARTBEAT/STATE_REQUEST를 막지 않는다. 한도 초과/응답 실패 시 연결을 닫고 실행을 차단한다.
- PNG는 최대 8 MiB/16,000,000 pixels, uploader는 진행 1개+대기 1개만 갖는다.
  Worker/전달 중 일시적인 PNG 복사가 추가로 존재한다. JSON에는 바이트/Base64를 넣지 않는다.
- Upload 콜백은 전용 스레드에서 실행한다. 반환 true는 HTTPS 전송 완료여야 한다.
  session/task/action/upload_id/상한/만료에 묶인 일회성 권한과 정상 TLS 검증은 구현자가 보장한다.
  임의 URL/Guest 파일 경로를 받는 API가 아니다. 콜백은 취소와 유한 deadline을 반드시 지켜야 한다.
- 업로드 실패/lease 만료/연결 단절/외부 stop은 이후 입력을 차단한다. 실패한 관찰 ID는 응답하지 않는다.
  Runtime의 캡처 ledger SUCCESS와 Host 업로드 성공은 서로 다른 상태다.
- 종료 동안 관리 요청을 처리한다. Windows API가 멈추거나 외부 uploader가 계약을 위반한 경우
  join 시간은 보장할 수 없다. Host가 grace_ms/긴급 종료 제한을 감시하고 프로세스/VM을 종료해야 한다.
- Workspace ID는 제공 모듈 기준 영숫자·하이픈·밑줄(1~64)만 허용한다. Host schema가 허용하는
  점/콜론 ID는 workspace 생성 시 거부한다. 자동 변환하지 않으며 이 차이는 Host 합의가 필요하다.

## 가져온 코드와 통합 수정

- `src/control/`: control_types, control_util, screen_capture, input_executor, action_scheduler.
- `src/runtime/`: session_workspace, sandbox_runtime.
- `src/gui_session.*`: 새 wire→Runtime adapter 및 uploader/결과/종료 흐름.
- `control_util`의 UUID/UTC 구현은 기존 SCRP 공통 함수에 위임한다. PNG SHA-256은 관찰 계약용이며
  Artifact 파일 해시를 Runner에 추가한 것이 아니다.
- Transport의 `::control`과 실행 모듈의 `runner::control`을 명시적으로 구분했다.
- 종료 대기 작업 수는 Scheduler 잠금 안에서 차단과 동시에 집계하여 실행 시작과의 경쟁을 없앴다.
- 원본 데모/README/AGENTS 지시를 제품 설정으로 복사하지 않았다. C++17 기능 최소 요구는
  제공 코드의 optional/filesystem에서 비롯되며 팀 전체 표준/컴파일러 버전 동결은 아니다.

## 검증 기록 (2026-09-28)

- MSYS2 UCRT64 GCC 16.2.0, CMake/Ninja Release 제품 빌드 통과.
- 일반 PowerShell `sandbox_runner.exe --help` 통과. EXE 약 3.87 MiB(기존 #13 약 3.46 MiB).
  PE import는 Windows 기본 DLL만 사용하며 MinGW/JsonCpp DLL 추가 없음. GUI 경로에는
  User32/GDI32/Ole32와 WIC COM이 필요하다. 실제 Sandbox 시작 시간은 미측정이다.
- CTest 11/11 통과: 기존 9개 + 제공 Runtime 76 assertions + GUI adapter 100 checks/7 시나리오.
  정상 관찰→클릭, 업로드 중 heartbeat, 실행 중 상태 조회, 입력 중 연결 실패/종료,
  capability/policy/Host grant 거부, 업로드 실패 시 관찰 미공개를 확인했다.
- 실제 WinHTTP Control WSS 회귀 10/10 통과(TLS trust/hostname/pin, 분할 프레임, 취소, timeout, 잘못된 session/payload/크기). 임시 CurrentUser Root 인증서 제거 확인.
- 새 응답 40개를 수정 없는 Host의 `scrp.validate.parse_and_validate`가 수용했다.
- 위 GUI 테스트는 **가짜 WorkerBackend/Upload와 메모리 WebSocket**이다. 실제 클릭·키보드 입력,
  실제 PNG 업로드, 실제 Sandbox↔Host GUI 왕복 성공으로 해석하면 안 된다.

기존 저장소 방침에 따라 테스트 소스와 테스트 CMake는 Git 제외 `tests/`에 있다.
현재 checkout에서 UCRT64 환경으로 재현한다:

```powershell
cmake -S . -B build/product -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/product
cmake -S tests -B build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
./build/product/sandbox_runner.exe --help
```

새 checkout의 제품 빌드는 `tests/` 없이 가능하다. GUI 운영 검증은 먼저 Host 승인/업로드 adapter를
연결한 뒤 읽기 전용 배포 매핑으로 Sandbox에 EXE만 배치한다. Sandbox 내부 테스트 앱에서
OBSERVE→클릭→한글 입력→스크롤→hotkey→STATE_REQUEST→TERMINATE를 실행하고,
입력 중 WSS 단절/lease 만료 시 후속 입력이 중지되는지 확인해야 한다.

추가로 원본 Host `feat/3-auth-tls`의 WSS Sender와 실제 Sandbox에서 관찰·클릭·한글 입력·
관리 요청을 검증했다. 테스트 adapter를 주입한 실제 모듈은 성공했고 기본 제품 CLI는
GUI 미지원으로 종료했다. 범위와 수집 오류는 [실제 Sandbox 검증 기록](sandbox-host-sender-validation.md)을 따른다.

**미검증/남은 결정:** 제품 Host 승인 및 HTTPS 업로드 계약·구현, Host WSS/auth 운영 연결,
다양한 DPI/UIPI·시작 비용, Control+Artifact+Telemetry 동시 lifecycle.
