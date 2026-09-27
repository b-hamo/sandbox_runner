# Sandbox Runner 프로젝트 구조

## 1. 목적과 문서 기준

이 문서는 현재 Runner의 코드 구조와 SCRP 통신을 연결할 후속 구조를 구분해 설명한다.
Issue #5에서 Artifact와 독립적인 Telemetry 전송 계층을 구현했다. Host의 확정 payload
스키마·인증 헤더는 외부 계약으로 주입한다. Issue #7에서 외부 Session Context 검증과
Telemetry 시작·READY 확인·종료를 제품 진입점에 연결했다. Control HELLO_ACK는 아직 미구현이다.
제품 실행 파일은 `sandbox_runner.exe` 하나이며, Control과 Artifact를 같은 실행 파일에 통합한다.
후속 구조에 적힌 디렉터리·클래스는 구현 위치에 대한 제안이며 현재 존재하는 코드가 아니다.

통신 기준은 **Secure CUA 통신 구조 설계안 v0.1**(2026-09-20,
`Secure_CUA_Protocol_W1_v0.1.pdf`)이다. 주요 근거는 다음과 같다.

| 명세 위치 | 내용 |
| --- | --- |
| 1쪽 | Control·Telemetry WSS 분리, 바이너리 HTTPS PUT, W2 스키마 동결 계획 |
| 5쪽 | Runner가 Host에 접속, endpoint, bootstrap·인증·채널 자격 |
| 6~8쪽 | Envelope, 메시지 타입, 필드 규칙 |
| 10~11쪽 | Artifact 승인·업로드·격리·검사·반출, Telemetry ACK·재전송 |

문서 버전 v0.1과 wire 초안의 `version: "1.0"`은 구분한다. 명세의 포트·시간·용량은
기본값이며, 타입별 필수·nullable 필드는 후속 확정 스키마를 따른다.

이후 합의된 Artifact 설계 변경을 우선 적용한다. Runner는 Defender 검사를 수행하지 않고,
SHA-256 계산·MIME 판별·수신 파일 크기 검증은 Host Quarantine에서 수행한다.
Runner는 크기 등 메타데이터를 **안정화 관찰 목적**으로만 사용한다.

## 2. 현재 구현 구조

아래는 현재 제품 코드와 문서의 구조다. 헤더는 구현 파일과 같은 디렉터리에 둔다.

```text
sandbox_runner/
├─ CMakeLists.txt
├─ README.md
├─ src/
│  ├─ main.cpp
│  ├─ runner_paths.h
│  ├─ session_context.h / session_context.cpp
│  ├─ runner_lifecycle.h / runner_lifecycle.cpp
│  ├─ protocol/scrp/
│  │  ├─ envelope.h / envelope.cpp
│  │  └─ telemetry_messages.h / telemetry_messages.cpp
│  ├─ transport/telemetry/
│  │  ├─ telemetry_client.h / telemetry_client.cpp
│  │  ├─ pending_event_store.h / pending_event_store.cpp
│  │  └─ winhttp_websocket.h / winhttp_websocket.cpp
│  └─ artifact/
│     ├─ output_watcher.h
│     ├─ output_watcher.cpp
│     ├─ candidate_detector.h
│     ├─ candidate_detector.cpp
│     ├─ file_stability.h
│     └─ file_stability.cpp
└─ docs/
   ├─ project-structure.md
   ├─ artifact-candidates.md
   └─ telemetry.md
```

`tests/`는 로컬 전용 검증 코드·스크립트·CMake의 위치다. Git에서 제외하며 제품 빌드는 이 폴더를 참조하지 않는다.
`build/`는 EXE·CMake 캐시·로컬 검증 산출물의 위치이며 Git에서 제외한다.
위 트리는 제품 구조를 설명하기 위한 것으로 개인 테스트 파일까지 나열하지 않는다.

| 파일 | 현재 책임 |
| --- | --- |
| `src/main.cpp` | 외부 Context 입력, Telemetry READY 이후 Output 준비·감시·후보 감지, 콘솔 종료 조정 |
| `src/runner_paths.h` | 기본 Output 경로 `C:\RunnerWorkspace\Output` 정의 |
| `src/session_context.*` | 기존 `telemetry::Context`를 `runner::SessionContext`로 재사용, 사전 검증·명시적 로컬 JSON 입력 |
| `src/runner_lifecycle.*` | 주입 Context로 Client 생성·시작, 취소 가능한 READY 대기, 실패 시 RAII 정리 |
| `src/artifact/output_watcher.*` | Windows 변경 알림의 재귀 감시, 경로 경계, 감시 오류·취소 처리 |
| `src/artifact/candidate_detector.*` | 이벤트 큐·worker, 파일별 debounce·관찰 일정, 변경 세대·후보 상태·무효화 관리 |
| `src/artifact/file_stability.*` | 허용된 일반 파일 접근, 경로 체인 검증, 쓰기 공유 제한, 파일 정보 관찰·비교 |
| `src/protocol/scrp/envelope.*` | 공통 Envelope, JSON 검증·직렬화, UUIDv4·nonce·UTC 시각 |
| `src/protocol/scrp/telemetry_messages.*` | 일반 SecurityEvent와 외부 TelemetrySchema 계약, HELLO·이벤트 생성 |
| `src/transport/telemetry/telemetry_client.*` | 전용 worker, 채널 바인딩, ACK 검증, 상태·재연결·재전송·종료 |
| `src/transport/telemetry/pending_event_store.*` | 미확인 이벤트 보관, 중복·충돌·용량 제한, 명시적 만료 |
| `src/transport/telemetry/winhttp_websocket.*` | WinHTTP 비동기 WSS, 인증 헤더 주입, TLS 검증, 프레임 재조립·취소 |
| `CMakeLists.txt` | 단일 Runner, `runner_telemetry` OBJECT와 `runner_lifecycle` STATIC 연결, 제품 빌드만 구성 |

기존 `output_watcher`를 새 감시기로 교체하지 않는다. 후보 상태는 현재
`CandidateDetector` 내부에서 관리하며 별도의 Candidate Registry·Publisher는 아직 없다.

## 3. 현재 처리 흐름과 인터페이스

```text
main.cpp
  ├─ --session-context 로컬 파일 → runner::SessionContext 검증
  ├─ start_telemetry() → Client worker → WSS → CHANNEL_HELLO/ACK → READY
  ├─ Output 경로 준비
  ├─ CandidateDetector 생성 → worker 시작
  └─ watch_output() 실행 → 호출 스레드에서 변경 알림 대기
         ↓ OutputChange
     CandidateDetector::submit() → 큐에만 전달
         ↓ worker
     파일별 debounce·변경 세대 관리
         ↓
     observe_file() / same_observation()
     경로 검증·쓰기 핸들 해제 확인·일정 간격의 메타데이터 비교
         ↓ 안정화 조건 충족
     CandidateStatus 콜백(state = candidate)
         ↓
     main.cpp의 ARTIFACT_CANDIDATE 콘솔 출력
```

`output_watcher`는 변경 알림을 수집한다. 파일을 후보로 판단하는 책임은
`CandidateDetector`와 안정화 관찰 코드에 있다. 감시 콜백에서 파일 관찰이나 네트워크 I/O를 수행하지 않는다.

`CandidateStatus`는 상대 경로, 파일 변경 세대, 상태, 오류, 설명을 전달한다.
상태는 `pending`, `stabilizing`, `candidate`, `unavailable`이다.
삭제·이름 변경·재수정은 기존 후보를 무효화하거나 새 세대의 관찰을 시작한다.
후속 소비자는 후보 이벤트뿐 아니라 이후 변경과 무효화도 반영해야 한다.

현재 `ARTIFACT_CANDIDATE`는 내부 상태 콜백과 로그이며 SCRP JSON이나 Host 전송이 아니다.
안정화는 관찰 기반 판단이므로 최종 쓰기 완료·안전성·파일의 불변성을 보장하지 않는다.
상세 조건과 제한은 [Artifact 후보 감지 문서](artifact-candidates.md)를 따른다.

종료 요청 시 `main.cpp`가 감시 I/O 종료를 기다리고 후보 worker를 중지·join한 뒤
Telemetry를 stop한다. 생산자를 먼저 중지하여 종료 중 새 작업이 유입되지 않게 한다.
Telemetry는 신규 enqueue를 거부하고 WSS I/O·콜백을 정리한 후 worker를 join한다.
미확인 Pending은 stop 후 진단 가능하고 Client 소멸 시 해제한다. ACK 성공으로 간주하거나 무한 drain하지 않는다.
예외 시에도 생성의 역순으로 후보 worker, Telemetry, 종료 이벤트를 정리한다.
감시 오류·누락이 드러나면 후보 상태도 무효화한다. Sandbox의 시작·강제 종료는 Host Runtime의 책임이다.

### Runner Session Context와 Telemetry 흐름 — 현재 구현

`src/session_context.h`의 `runner::SessionContext`는 `telemetry::Context`의 alias다.
기존 `scrp::SessionContext`의 ID·Runtime generation, endpoint·TLS trust, credential provider,
외부 `TelemetrySchema`를 그대로 사용한다. Context나 인증 정보를 새로 생성하지 않는다.
`start_telemetry()`는 필수 값·credential을 사전 검증하고 Client를 생성·시작하며 READY까지 기다린다.
초기 연결 재시도 소진·기본 30초 READY 제한·종료 요청은 명시적 실패로 반환하며 Client를 정리한다.
기존 Client의 재연결 정책은 유지한다. READY 이후 장애의 Host 정책·Coverage 연결은 후속 작업이다.

현재 실행 파일은 `--session-context <파일>`을 필수로 받는다. 로컬 입력 형식·trust·handshake
계약은 [Telemetry 문서](telemetry.md)의 Session Context 입력 절차를 따른다.
이는 Control/Host wire 규약이 아니며 Control HELLO_ACK, credential 발급·갱신은 구현하지 않는다.
향후 Control은 `runner::SessionContext`와 실제 `TelemetrySchema`를 직접 주입하면 된다.

```text
호출자: Host SessionContext / endpoint / credential provider / TLS trust / TelemetrySchema 주입
  ├─ enqueue(SecurityEvent) → 제한된 PendingEventStore (네트워크 I/O 없음)
  └─ start() → 전용 worker → WSS → CHANNEL_HELLO → CHANNEL_ACK 검증 → READY
                                 ↓
                   Pending 첫 이벤트 → SECURITY_EVENT → EVENT_ACK 검증 → Pending 제거
                                 ↓ 연결 또는 ACK 실패
                   RECONNECTING → 새 WSS·CHANNEL 바인딩 → 미확인 이벤트 새 Envelope로 재전송
  stop() → STOPPING → 비동기 I/O 취소·콜백 회수·worker join → STOPPED
```

`snapshot()`은 상태, Pending 개수·바이트, 거부·만료·오류 누계와 진단을 제공한다.
만료·초과는 Coverage 저하로 노출하며 임의로 SECURITY_EVENT 스키마를 만들어 보내지 않는다.
Host의 Action 차단 정책이나 Coverage wire 연결은 후속 책임이다.
제품 실행 시 위 lifecycle에서 Telemetry를 시작한다. Artifact 콜백은 기존 콘솔 출력만 수행하며
Candidate → SECURITY_EVENT 연결은 구현하지 않았다.

## 4. 후속 확장 구조 제안 — 미구현

실제 작업 이슈와 공용 인터페이스 합의에 따라 필요한 파일부터 추가한다.
아래 구조를 맞추기 위해 기존 파일을 이동하거나 빈 계층을 미리 만들지 않는다.

```text
src/
├─ main.cpp                       공통 시작·종료 및 모듈 연결
├─ runner_paths.h
├─ session_context.*              [현재] 공용 Context alias·검증·외부 입력
├─ runner_lifecycle.*             [현재] Telemetry 시작·READY 확인·실패 정리
├─ artifact/                      현재 감시·안정화·후보 감지 유지
│  ├─ output_watcher.*
│  ├─ candidate_detector.*
│  ├─ file_stability.*
│  └─ [후속] 후보 ID·보고·승인된 전송 요청 연결
├─ control/                       [후속] 화면 관찰·GUI 입력 실행
├─ protocol/
│  └─ scrp/                       [현재] Envelope·Telemetry 타입 / [후속] Host 확정 스키마
└─ transport/
   ├─ control/                    [후속] 제어 WSS 연결·송수신
   ├─ telemetry/                  [현재] 이벤트 WSS 연결·ACK·재전송
   └─ [후속] 승인된 HTTPS 업로드
```

공용 헤더를 `include/`로 분리할지는 팀 규칙과 실제 외부 노출 필요에 따라 정한다.
현재 `.h`와 `.cpp` 배치를 유지해도 되며, `.hpp`로 변경할 필요는 없다.

## 5. 계층별 책임과 연결 상태

### Artifact

현재 감지 기능에 후보 식별과 후속 요청 연결을 추가할 위치다.
필요하면 Candidate Registry와 Publisher로 나눌 수 있으나 지금 별도 구현은 없다.
Runner의 로컬 후보 기록과 Host Artifact Broker의 후보 등록·승인 기록은 서로 다른 책임이다.

Publisher를 추가한다면 내부 상태를 Protocol의 이벤트 타입으로 변환하는 역할을 맡긴다.
감시 코드 안에서 WebSocket 연결·인증·재전송을 직접 처리하지 않는다.
후보의 식별자 발급·유효기간·무효화 표현은 Host와의 계약에 맞춰 연결한다.

### Control 실행 기능: `src/control/`

화면 관찰, 마우스·키보드·스크롤 등 GUI 작업을 실행한다.
네트워크 전송 계층인 `src/transport/control/`과 구분한다.
Control과 Artifact는 각각의 내부 구현에 직접 의존하지 않고 공용 계약과 `main.cpp`에서 연결한다.

### SCRP Protocol: `src/protocol/scrp/`

Envelope·메시지 타입·payload·직렬화와 검증 규칙을 공통으로 관리한다.
Control·Telemetry에서 같은 규칙을 사용하며 Artifact 전용 코드에 Envelope를 중복 정의하지 않는다.
소켓 연결이나 GUI 실행은 담당하지 않는다.
현재 공통 Envelope와 Telemetry 메시지 생성이 구현되어 있다. 닫힌 payload 검증과 ACK 성공
상태 해석은 호출자가 제공하는 `TelemetrySchema` 책임이다. 테스트 전용 스키마는
Git에서 제외된 로컬 검증 코드에만 존재하며 제품 기본 스키마가 아니다.

### Control Transport: `src/transport/control/`

제어 채널 연결·인증·송수신을 담당하고 메시지를 해당 기능으로 전달한다.
명세상 `HELLO / HELLO_ACK`, `OBSERVE / OBSERVE_RESULT`, `ACTION_REQUEST / ACK / ACTION_RESULT`,
`STATE_REQUEST / STATE_RESULT`, `HEARTBEAT / ALIVE`, `ARTIFACT_REQUEST / ARTIFACT_RESULT`,
`TERMINATE / TERMINATE_RESULT`가 이 채널을 사용한다.

### Telemetry Transport: `src/transport/telemetry/`

Runner에서 Host Security Backend로 이벤트를 전달한다.
`CHANNEL_HELLO / CHANNEL_ACK`로 채널을 바인딩하고 `SECURITY_EVENT / EVENT_ACK`를 처리한다.
Artifact 외의 보안 이벤트도 같은 전송 계층을 사용한다.
현재 재사용 가능한 `TelemetryClient`로 구현되어 있다. 같은 연결에서 한 이벤트씩 ACK를
기다린다. 이벤트를 READY 전에 큐에 넣을 수 있고 `start()` 전에도 보관한다.

Host는 이벤트를 저장한 후 `EVENT_ACK`를 보낸다. 재연결 시 미확인 이벤트를 새 Envelope로
재전송하며 같은 `event_id`는 유지해 Host가 중복 저장을 방지한다.
연결별 `sequence_number`와 원래 이벤트의 관찰 시각은 구분한다.
이벤트 저장 확인은 파일 업로드 승인이나 안전 판정이 아니다.

ACK 대기·재연결·미확인 이벤트 버퍼는 전송 계층에서 관리한다.
명세의 버퍼 기본값은 10 MiB 또는 5분 중 먼저 도달하는 한도이며,
초과·유실은 Coverage 저하로 보고한다. 무제한 버퍼나 묵시적 폐기는 사용하지 않는다.
현재 저장은 메모리 전용이다. 재연결은 1·2·4·8초와 jitter 후 중단하며 `DISCONNECTED`로
외부 복구 필요 상태를 노출한다. ACK 성공 시 연속 재시도 횟수를 초기화한다.
세션·Runtime generation은 생성 시 고정하며 변경 시 새 Client를 만들어야 한다.
구체적인 API·제한·검증 절차는 [Telemetry 문서](telemetry.md)를 따른다.

## 6. 접속·인증·바이너리 전송

Runner가 Host endpoint로 먼저 연결한다. Host는 수립된 양방향 Control 연결로 요청을 보낸다.
endpoint는 Host Lifecycle Manager가 시작 설정으로 주입한다.
Sandbox에서 `localhost`는 Guest 자신이므로 Host 주소로 사용하지 않는다.

| 용도 | SCRP v0.1 기본 endpoint |
| --- | --- |
| Control | `wss://<host>:17443/scrp/v1/control` |
| Telemetry | `wss://<host>:17443/scrp/v1/telemetry` |
| Observation 업로드 | `https://<host>:17443/scrp/v1/observations/<upload_id>` |
| Artifact 업로드 | `https://<host>:17443/scrp/v1/artifacts/<upload_id>` |

`17443`은 설정 가능한 기본 포트다. 같은 포트를 사용해도 연결·인증 Scope·큐·제한은 분리한다.
최종 Host 연동에서는 Control의 bootstrap 인증과 HELLO_ACK로 별도 채널 자격을 전달할 예정이다.
현재는 해당 Control 경로 대신 외부 Context를 명시적으로 주입해 Telemetry를 연결한다.
TLS 인증서 검증을 끄거나 평문으로 자동 전환하지 않는다. 토큰은 로그나 URL에 넣지 않는다.
현재 WSS는 Windows 인증서 저장소의 체인·호스트명 검증과 최소 TLS 1.2를 사용한다.
`TlsTrust::leaf_sha256`는 선택적 추가 pin이며 기본 인증서 검증을 대체하지 않는다.
Bootstrap 전용 신뢰 앵커를 Windows 저장소에 배치하는 일은 외부 Runtime 책임이다.
Telemetry 인증 헤더 이름·값·만료는 외부 credential provider에서 받는다. Bearer 등 특정
헤더 형식은 기본값으로 정하지 않으며 redirect·쿠키·자동 HTTP 인증·평문 fallback은 사용하지 않는다.

Control·Telemetry JSON에 파일 바이트나 대용량 Base64를 넣지 않는다.
파일은 Host가 승인한 endpoint와 파일·세션·상한·만료에 한정된 권한으로 HTTPS PUT 전송한다.
Guest가 지정한 임의 URL이나 임의 경로를 그대로 사용하는 범용 업로드 API를 만들지 않는다.

## 7. 후보 보고와 파일 반출 — 후속 흐름

```text
Runner 내부 후보 이벤트                         [현재 구현]
    ↓ 후보 식별·SCRP 변환                        [이하 미구현]
SECURITY_EVENT(category = ARTIFACT_CANDIDATE)
    ↓ Telemetry WSS
Host 이벤트 저장 → EVENT_ACK
    ↓ 별도의 후보 승인·전송 요청
Host Artifact Broker: artifact_id·upload_id·업로드 권한 발급
    ↓ Control WSS의 ARTIFACT_REQUEST
Runner: 현재 후보·파일 상태·허용 경로 재검증
    ↓ HTTPS PUT
Host Quarantine: 비공개 경로에 파일 수신
    ↓
Host: 동일한 불변 바이트의 악성코드 검사 및 SHA-256·MIME·크기 검증
    ↓ 최종 승인
Safe Results 반출 및 EXPORTED 기록
```

`ARTIFACT_CANDIDATE`는 독립된 최상위 SCRP 메시지 타입이 아니라 `SECURITY_EVENT`의 category다.
Host의 수신 완료와 Runner의 업로드 성공은 안전 판정이 아니다.
검사기 오류·미설치·시간 초과 시 Host는 SAFE로 처리하지 않고 QUARANTINED 상태를 유지한다.

Host는 Guest 파일명·경로를 저장 경로로 직접 사용하지 않으며, 최종 검사와 반출에도 같은 바이트를 사용한다.
Runner 내부 Output을 Host의 쓰기 가능한 공유 폴더로 매핑하지 않는다.

## 8. 내부 상태와 wire 규약의 구분

| 개념 | 의미 및 현재 상태 |
| --- | --- |
| `CandidateStatus::generation` | 현재 구현된 로컬 파일 변경 세대. 후보 변경·무효화 추적용 |
| SCRP Envelope `generation` | Host가 관리하는 Runtime 재생성 세대. 위 값으로 대체하면 안 됨 |
| `candidate_id` | 후보를 전송 요청과 연결할 식별자. 현재 내부 상태에는 없음 |
| `event_id` | 현재 `SecurityEvent`의 ID. Pending 중복·ACK 추적에 사용. Artifact 상태와는 미연결 |
| `artifact_id` / `upload_id` | Host Artifact Broker가 발급할 파일 등록·업로드 식별자 |
| `message_id` / `sequence_number` | Envelope 및 연결·송신 방향별 전송 식별. 파일 세대와 별개 |

v0.1의 7쪽에는 후보 메타데이터에 해시가 포함되어 있다. 최신 설계에서는 Runner가 SHA-256을
계산하지 않으므로, 후속 스키마에 이 변경을 반영해야 한다. 해시 필드를 생략할지 nullable로 둘지는
Host와 합의하고 문서화한다. 임의의 해시나 빈 값을 유효한 해시처럼 전송하지 않는다.
이 문서는 스키마를 임의로 확정하거나 기존 Envelope 필드를 다른 의미로 재사용하지 않는다.

## 9. 빌드·배포 및 문서 관리

Host의 MSYS2 UCRT64 환경에서 CMake/Ninja로 Windows EXE를 빌드한다.
C++ 표준과 도구 버전은 팀 결정에 따르며 이 문서에서 새로 고정하지 않는다.
JsonCpp는 Host 빌드 의존성이고 `jsoncpp_static`으로 연결한다. WinHTTP·BCrypt·Crypt32는
Windows 기본 DLL을 사용한다. 검증 환경 버전은 Telemetry 문서에 기록하며 팀 동결값으로 간주하지 않는다.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Sandbox에는 Runner와 필요한 런타임 파일만 배치한다. 빌드 도구 설치를 전제로 하지 않는다.
Sandbox 자동 시작·bootstrap·네트워크 정책은 Host Runtime 담당 영역이며 Runner 제품 코드에 넣지 않는다.

사용자 요청에 따라 테스트 소스·스크립트·테스트 CMake는 로컬 `tests/`에만 보관하고 Git에서 제외한다.
공유 CMake는 테스트 파일·타깃·옵션을 참조하지 않는다. 새 checkout에서 제품만 빌드할 수 있다.
기존 테스트의 검증 결과는 이력으로 유지하며 로컬 테스트 EXE는 제품 배포 대상이 아니다.

README는 프로젝트 개요·빌드 진입점, 이 문서는 구조와 책임 경계,
[artifact-candidates.md](artifact-candidates.md)는 Artifact 동작·제한·검증 절차를 관리한다.
후속 계층을 구현하면 이 문서의 미구현 표기와 현재 트리를 함께 갱신한다.

## 10. 다음 작업

1. 구현된 공용 Session Context에 실제 Host SCRP 스키마를 제공한다. 파일 세대·Runtime 세대 및 후보 해시 정책을 구분한다.
2. Control handshake를 구현하고 HELLO_ACK의 Telemetry 자격·확정 스키마를 현재 Client에 연결한다.
3. 내부 후보 상태를 SECURITY_EVENT로 연결하고 ACK·재전송·무효화 계약을 검증한다.
4. 승인된 ARTIFACT_REQUEST 처리와 제한된 HTTPS 업로드를 구현한다.
5. Host Quarantine의 검사·승인·반출과 연결해 전체 흐름을 검증한다.

이 작업들은 현재 후보 감지 구현의 완료 여부와 분리해 후속 이슈로 관리한다.
