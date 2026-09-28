# Sandbox Runner 프로젝트 구조

## 1. 목적과 문서 기준

이 문서는 현재 Runner의 코드 구조와 SCRP 통신을 연결할 후속 구조를 구분해 설명한다.
Issue #5에서 Artifact와 독립적인 Telemetry 전송 계층을 구현했다. Host의 확정 payload
스키마·인증 헤더는 외부 계약으로 주입한다. Issue #7에서 외부 Session Context 검증과
Telemetry 시작·READY 확인·종료를 제품 진입점에 연결했다.
Issue #13에서 Control HELLO/HELLO_ACK·요청 수신·검증·분배 모듈을 추가했다.
Host `feat/1-host-sender`의 fed305b 스키마 adapter와 응답 송신을 추가하고,
main의 `--control-context` 관리 모드에 연결했다. Artifact/Telemetry 모드와는 별도로 선택한다.
Issue #14에서 제공 GUI 실행 모듈과 `GuiSession` 내부 adapter·비동기 결과 경로를 통합했다.
기존 내부 주입 경로에 더해 #17에서 원본 Host bootstrap용 실행 경로를 추가했다. [GUI 연동 문서](gui-integration.md)를 따른다.
Issue #17에서 제품 Control owner와 제한된 HTTPS PNG 업로더를 추가했다.
Host `feat/19-observation-upload`의 `6055cc6`용 `--host-bootstrap` 모드를 추가했다.
실제 Output 감시와 GUI·HTTPS 업로드를 연결한다. 시작/입력 허용 판단은 인증된 Host Broker가 담당한다.
기존 `--control-context`는 관리 모드를 유지한다. [현재 계약과 검증](host-observation-integration.md)을 따른다.
Issue #9에서 Candidate 콜백과 TelemetryClient 사이에 `CandidateTelemetry` 변환 계층을 연결했다.
Issue #11에서 `artifact-candidate-v1` payload·ACK 계약안과 제품 `ArtifactCandidateAdapter`를 구현했다.
로컬 Context의 명시적 `event_contract` 선택으로 실제 후보 전송을 활성화한다. 미선택 시 기존
handshake-only 동작을 유지한다. Host 공유·합의 상태는 [계약 문서](artifact-candidate-contract.md)에 기록한다.
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
Artifact 파일의 SHA-256 계산·MIME 판별·수신 파일 크기 검증은 Host Quarantine에서 수행한다.
GUI 관찰 PNG의 SHA-256은 별도 OBSERVE_RESULT 계약에 따라 Runner가 계산한다.
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
│  ├─ control_session.h / control_session.cpp
│  ├─ gui_session.h / gui_session.cpp
│  ├─ gui_product.h / gui_product.cpp
│  ├─ host_gui.h / host_gui.cpp
│  ├─ control/ (control_types, control_util, screen_capture, input_executor, action_scheduler)
│  ├─ runtime/ (session_workspace, sandbox_runtime)
│  ├─ candidate_telemetry.h / candidate_telemetry.cpp
│  ├─ artifact_candidate_adapter.h / artifact_candidate_adapter.cpp
│  ├─ protocol/scrp/
│  │  ├─ envelope.h / envelope.cpp
│  │  ├─ telemetry_messages.h / telemetry_messages.cpp
│  │  ├─ host_sender_schema.h / host_sender_schema.cpp
│  │  └─ artifact_candidate.h / artifact_candidate.cpp
│  ├─ transport/telemetry/
│  │  ├─ telemetry_client.h / telemetry_client.cpp
│  │  ├─ pending_event_store.h / pending_event_store.cpp
│  │  └─ winhttp_websocket.h / winhttp_websocket.cpp
│  ├─ transport/control/
│  │  └─ control_receiver.h / control_receiver.cpp
│  ├─ transport/observation/
│  │  └─ https_uploader.h / https_uploader.cpp
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
   ├─ artifact-candidate-contract.md
   ├─ control-receiver.md
   ├─ host-sender-integration.md
   ├─ gui-integration.md
   ├─ gui-product-contract-proposal.md
   ├─ gui-product-validation.md
   └─ telemetry.md
```

`tests/`는 로컬 전용 검증 코드·스크립트·CMake의 위치다. Git에서 제외하며 제품 빌드는 이 폴더를 참조하지 않는다.
`build/`는 EXE·CMake 캐시·로컬 검증 산출물의 위치이며 Git에서 제외한다.
위 트리는 제품 구조를 설명하기 위한 것으로 개인 테스트 파일까지 나열하지 않는다.

| 파일 | 현재 책임 |
| --- | --- |
| `src/main.cpp` | 명시적 Artifact/Telemetry 또는 Control 모드 선택, 외부 Context 입력, 콘솔 종료 조정 |
| `src/runner_paths.h` | 기본 Output 경로 `C:\RunnerWorkspace\Output` 정의 |
| `src/session_context.*` | Telemetry Context 및 별도 Control Context의 명시적 로컬 JSON 입력·자격/TLS 검증 |
| `src/runner_lifecycle.*` | 주입 Context로 Client 생성·시작, 취소 가능한 READY 대기, 실패 시 RAII 정리 |
| `src/control_session.*` | Control 수신 worker·stop/join, 기본 관리 모드 및 선택적 GuiSession 연결 |
| `src/gui_session.*` | 검증 요청 변환, Host grant·lease, ACK/결과 큐, scoped uploader 주입, 비동기 종료 |
| `src/host_gui.*` | 원본 Host bootstrap 검증, 세션 Output 감시 시작·건강 상태, Host6055 GUI·업로더 연결과 종료 |
| `src/gui_product.*` | 제품 Control owner, 서비스 사전 검증·probe 준비·GUI/uploader 수명 관리, 미설정 시 관리 모드 |
| `src/transport/observation/https_uploader.*` | 고정 HTTPS origin/경로, 요청에 묶인 일회성 권한, PNG 상한·만료·취소·완료 상태 검증 |
| `src/control/*` | Windows 캡처·입력, 직렬 Scheduler·ledger·좌표 관찰 유효성 |
| `src/runtime/*` | Guest workspace 및 실행 권한·lease·차단, Host VM Manager와 구분 |
| `src/protocol/scrp/host_sender_schema.*` | Host fed305b 초안 호환 HELLO/ACK·요청·제품 응답 검증, negotiated 메시지 한도 |
| `src/candidate_telemetry.*` | Candidate 콜백의 중복·무효화 추적, 외부 계약으로 SecurityEvent 변환, 기존 Client enqueue·거부 진단 |
| `src/artifact_candidate_adapter.*` | 기존 handshake schema를 유지하면서 CandidateEventContract·TelemetrySchema의 후보 payload·ACK 연결 |
| `src/artifact/output_watcher.*` | Windows 변경 알림의 재귀 감시, 경로 경계, 감시 오류·취소 처리 |
| `src/artifact/candidate_detector.*` | 이벤트 큐·worker, 파일별 debounce·관찰 일정, 변경 세대·후보 상태·무효화 관리 |
| `src/artifact/file_stability.*` | 허용된 일반 파일 접근, 경로 체인 검증, 쓰기 공유 제한, 파일 정보 관찰·비교 |
| `src/protocol/scrp/envelope.*` | 공통 Envelope, JSON 검증·직렬화, UUIDv4·nonce·UTC 시각 |
| `src/protocol/scrp/telemetry_messages.*` | 일반 SecurityEvent와 외부 TelemetrySchema 계약, HELLO·이벤트 생성 |
| `src/protocol/scrp/artifact_candidate.*` | artifact-candidate-v1 payload 생성·닫힌 스키마 검증, 저장 성공·거부 ACK 해석 |
| `src/transport/telemetry/telemetry_client.*` | 전용 worker, 채널 바인딩, ACK 검증, 상태·재연결·재전송·종료 |
| `src/transport/telemetry/pending_event_store.*` | 미확인 이벤트 보관, 중복·충돌·용량 제한, 명시적 만료 |
| `src/transport/telemetry/winhttp_websocket.*` | WinHTTP 비동기 WSS, 인증 헤더 주입, TLS 검증, 프레임 재조립·취소 |
| `src/transport/control/control_receiver.*` | Control handshake·요청 검증·핸들러 분배·응답 상관관계/송신 sequence, 취소·실패·소켓 정리 |
| `CMakeLists.txt` | 단일 Runner, `runner_telemetry` OBJECT와 `runner_lifecycle`/`runner_control`/`runner_gui` STATIC, 제품 빌드만 구성 |

기존 `output_watcher`를 새 감시기로 교체하지 않는다. 후보 상태는 현재
`CandidateDetector` 내부에서 관리한다. `CandidateTelemetry`는 보고한 관찰의 ID·세대만 추적하며
파일 등록·승인용 Candidate Registry는 아니다.

## 3. 현재 처리 흐름과 인터페이스

`--control-context` 선택 시에는 `main → load_control_context → run_product_control_session → run_control_session → Receiver`로
연결한다. 수신 worker는 HELLO_ACK 이후 요청을 검증하고 관리 응답을 송신한다.
TERMINATE_RESULT 송신 또는 콘솔 취소 후 socket close·worker join·종료 이벤트 정리 순서로 끝난다.
`--host-bootstrap`은 Output 감시 준비 → WSS HELLO → HELLO_ACK → GUI Worker 시작 → Host의 STATE/OBSERVE/HEARTBEAT 시작 검증 순서다.
Host가 READY 이후 입력을 발행하며, 요청받은 OBSERVE에만 PNG를 전송한다. 종료/단절 또는 감시 오류는 입력을 차단하고 Worker·전송·감시 스레드를 정리한다.
GUI 모드의 Output 감시는 현재 알림 수와 건강 상태를 관리하며 Candidate/Telemetry 반출 흐름은 별도다.

CLI 기본 관리 모드의 GUI Worker와 Coverage는 비활성이며 Runtime READY를 주장하지 않는다.
신뢰된 호출자가 GuiSession을 주입하면 WSS 수신 → 직렬 GUI Worker → bounded 결과 큐 → WSS 응답으로 연결한다.
캡처는 전용 uploader의 성공 확인 후 메타데이터를 응답하고, 연결 단절은 Runtime을 즉시 block한다.
시작/종료 흐름과 Host 계약 차이는 [GUI 연동 문서](gui-integration.md)에 상세히 기록한다.
제품 owner에 `GuiProductServices`가 주입되면 실제 캡처와 준비 callback을 거쳐 위 GUI 경로를 시작한다.
현재 CLI는 services를 주입하지 않는다. 합의된 Host adapter/factory와 최신 Host의 준비 단계
관찰·입력 허용 전이는 미구현이며, 내부 연결점 존재를 제품 활성화로 간주하지 않는다.
uploader는 GuiSession보다 먼저 생성되고 나중에 파괴되어 전송 작업 종료까지 수명을 유지한다.
아래 기존 Artifact/Telemetry 흐름은 `--session-context` 모드에서만 실행된다.

```text
main.cpp
  ├─ --session-context 로컬 파일 → runner::SessionContext 검증
  │    └─ event_contract 선택 → InjectedHandshake를 ArtifactCandidateAdapter로 감쌈
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
     main.cpp 콜백 → CandidateTelemetry::submit() + 기존 콘솔 출력
         ↓ candidate 상태만 신규 후보 이벤트로 변환
     CandidateEventContract::candidate_payload() → ArtifactCandidateAdapter
         ↓ scrp::artifact_candidate_payload() (명시적으로 선택한 계약안)
         ↓ SecurityEvent(event_id, observed_at, category=ARTIFACT_CANDIDATE)
     TelemetryClient::enqueue() → 기존 Pending → SECURITY_EVENT Envelope → WSS / EVENT_ACK
```

`output_watcher`는 변경 알림을 수집한다. 파일을 후보로 판단하는 책임은
`CandidateDetector`와 안정화 관찰 코드에 있다. 감시 콜백에서 파일 관찰이나 네트워크 I/O를 수행하지 않는다.

`CandidateStatus`는 상대 경로, 파일 변경 세대, 상태, 오류, 설명을 전달한다.
상태는 `pending`, `stabilizing`, `candidate`, `unavailable`이다.
삭제·이름 변경·재수정은 기존 후보를 무효화하거나 새 세대의 관찰을 시작한다.
후속 소비자는 후보 이벤트뿐 아니라 이후 변경과 무효화도 반영해야 한다.

`ARTIFACT_CANDIDATE`는 외부 계약이 주입되면 기존 SECURITY_EVENT 경로로 전송한다.
로컬 Context에서 `event_contract: "artifact-candidate-v1"`을 선택하면 제품 adapter가
후보 계약·EVENT_ACK를 제공한다. 선택하지 않으면 `InjectedHandshake`만 사용해 기존 미전송 진단을 남긴다.
`ArtifactCandidateAdapter`는 기존 `TelemetrySchema`와 `runner::CandidateEventContract`를 함께 구현한다.
`main.cpp`는 Context의 schema에서 해당 계약을 얻으며, 없는 경우 payload를 추측하지 않는다.
안정화는 관찰 기반 판단이므로 최종 쓰기 완료·안전성·파일의 불변성을 보장하지 않는다.
상세 조건과 제한은 [Artifact 후보 감지 문서](artifact-candidates.md)를 따른다.

종료 요청 시 `main.cpp`가 감시 I/O 종료를 기다리고 후보 worker를 중지·join한 뒤
Telemetry를 stop한다. 생산자를 먼저 중지하여 종료 중 새 작업이 유입되지 않게 한다.
Telemetry는 신규 enqueue를 거부하고 WSS I/O·콜백을 정리한 후 worker를 join한다.
미확인 Pending은 stop 후 진단 가능하고 Client 소멸 시 해제한다. ACK 성공으로 간주하거나 무한 drain하지 않는다.
변환 계층은 Telemetry 이후, CandidateDetector 이전에 생성한다. 콜백은 변환 계층을 참조하며
후보 worker join 후에는 더 이상 호출되지 않는다. 예외 시에도 생성의 역순으로 후보 worker,
변환 계층, Telemetry, 종료 이벤트를 정리한다. 변환 계층 자체는 worker나 네트워크를 소유하지 않는다.
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
이는 Control/Host wire 규약이 아니며 Control HELLO_ACK의 자격 추출, credential 발급·갱신은 구현하지 않는다.
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
제품 실행 시 위 lifecycle에서 Telemetry를 시작한다. Artifact 콜백은 변환 계층과 기존 콘솔 출력으로
전달한다. 외부 계약 미주입·변환 실패·enqueue 거부는 고정 진단으로 노출하고 성공으로 처리하지 않는다.

### Candidate 보고 계약과 상태

`CandidateObservation`은 Output 기준 UTF-8 상대 경로, 로컬 `file_generation`, `event_id`,
`observed_at`을 외부 계약에 전달하는 내부 타입이다. UUID와 UTC 시각은 기존 SCRP 유틸리티를
사용하며, payload의 event_id·observed_at 일치와 ARTIFACT_CANDIDATE category를 확인한 뒤 enqueue한다.
Issue #11 계약안은 `event_id`, `observed_at`, `category`, `relative_path` 네 필드만 전송한다.
별도 candidate_id·파일 세대는 전송하지 않는다. 세션 정보는 기존 Envelope가 붙인다.

- 동일 경로(기존 감지기와 같은 Windows 대소문자 비교)·세대는 enqueue 성공 후 ACK가 끝나도 다시 보고하지 않는다.
- pending·stabilizing은 신규 후보 이벤트를 생성하지 않는다. 쓰기 핸들 경합은 안정화 재시도 상태로 유지한다.
- 재수정·삭제·rename은 기존 관찰을 로컬 무효화하고, 새 세대가 안정화되면 새 event_id로 보고한다.
- 이미 보고된 후보의 무효화는 `invalidation_payload(previous, cause, event_id, observed_at)`에 위임한다.
  null을 반환하면 wire 미합의를 진단하고 로컬 무효화만 수행한다. 임의의 무효화 category를 만들지 않는다.
  artifact-candidate-v1 adapter는 항상 null을 반환한다. 무효화 wire 전송은 #11 범위 밖이다.
- 전역 unavailable(감시 오류·중지·worker 실패)은 모든 관찰을 무효화하고 추가 입력을 거부한다.
- 경로별 상태 한도는 기본 4096이며 CandidateDetector 기본 한도와 같다. 사용자 지정 시 두 한도를 맞춘다.
  이전 세대의 관찰 payload나 파일 바이트는 보관하지 않는다.
- READY 이전·재연결·ACK·재전송은 기존 Pending 처리에 맡긴다. 거부 이벤트의 자동 재시도 큐는 추가하지 않는다.
  같은 콜백이 재전달되면 거부된 관찰의 ID·시각을 유지해 다시 시도할 수 있다.
- Pending에 이미 들어간 이벤트는 과거 관찰 기록이다. 로컬 무효화가 이를 취소하거나 업로드를 승인하지 않는다.
  무효화 enqueue 실패·종료 시 미확인 Pending은 전달 보장이 없으며 후속 Host 정책이 필요하다.

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
├─ control/                       [현재] 화면 관찰·GUI 입력·직렬 실행
├─ runtime/                       [현재] Guest workspace·권한·lease (Host VM Manager 아님)
├─ protocol/
│  └─ scrp/                       [현재] Envelope·Telemetry 타입 / [후속] Host 확정 스키마
└─ transport/
   ├─ control/                    [현재] handshake·수신·응답·비동기 Worker 연결
   ├─ telemetry/                  [현재] 이벤트 WSS 연결·ACK·재전송
   └─ [후속] 승인된 HTTPS 업로드
```

공용 헤더를 `include/`로 분리할지는 팀 규칙과 실제 외부 노출 필요에 따라 정한다.
현재 `.h`와 `.cpp` 배치를 유지해도 되며, `.hpp`로 변경할 필요는 없다.

## 5. 계층별 책임과 연결 상태

### Artifact

현재 감지 기능은 전송 계층에 의존하지 않는다. Runner 조립 계층의 `CandidateTelemetry`가
후보 보고를 연결하며 승인된 파일 요청 연결은 후속 작업이다.
Runner의 로컬 후보 기록과 Host Artifact Broker의 후보 등록·승인 기록은 서로 다른 책임이다.

`CandidateTelemetry`는 외부 계약을 통해 내부 상태를 기존 Protocol의 이벤트 타입으로 변환한다.
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
상태 해석은 주입된 `TelemetrySchema` 책임이다. Issue #11의 명시적 선택은 제품
artifact-candidate-v1 validator를 사용하며 기본 선택 없이 자동 적용하지 않는다. 별도 테스트 전용 스키마는
Git에서 제외된 로컬 검증 코드에만 존재하며 제품 기본 스키마가 아니다.

### Control Transport: `src/transport/control/`

`control::Receiver`가 제어 WSS 연결·HELLO handshake·요청 수신·검증·분배를 담당한다.
외부 Schema가 닫힌 payload를 검증하고 등록된 핸들러만 호출한다. 응답 batch를 검증하고 송신하며 자동 재연결은 없다.
호출자가 수신 스레드와 stop/join을 소유하며, Receiver는 종료·실패 시 소켓을 정리한다.
main의 명시적 Control 모드에서 기동한다. 사용법·검증·한도는 [수신부 문서](control-receiver.md)를 따른다.

Issue #14의 GuiSession이 비동기 Worker 연결과 실제 Action 상태 조회를 담당한다.
Receiver의 Hooks::poll은 ACK 송신 이후 완료를 처리하고, disconnected는 Runtime을 차단한다.
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
EVENT_ACK는 보안 이벤트 저장 확인이며, Artifact Broker의 Candidate 등록 완료를 의미하지 않는다.
현재 Client는 검증된 ACK로 Pending을 제거할 뿐 Candidate 등록 상태를 변경하지 않는다.
변환 계층의 `accepted`도 로컬 enqueue 수용 여부이며 Host ACK나 후보 등록 완료 상태가 아니다.
Broker 등록 결과 확인과 등록 실패 재처리 계약은 후속 작업이다.
이벤트 저장 확인은 파일 업로드 승인이나 안전 판정도 아니다.

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
현재 Telemetry 모드는 외부 Context를 명시적으로 주입한다. 별도 Control 모드는 HELLO_ACK의
채널 자격 형식을 검증하지만 Telemetry를 시작하거나 재연결 자격으로 사용하지 않는다.
TLS 인증서 검증을 끄거나 평문으로 자동 전환하지 않는다. 토큰은 로그나 URL에 넣지 않는다.
현재 WSS는 Windows 인증서 저장소의 체인·호스트명 검증과 최소 TLS 1.2를 사용한다.
`TlsTrust::leaf_sha256`는 선택적 추가 pin이며 기본 인증서 검증을 대체하지 않는다.
Bootstrap 전용 신뢰 앵커를 Windows 저장소에 배치하는 일은 외부 Runtime 책임이다.
Telemetry 인증 헤더 이름·값·만료는 외부 credential provider에서 받는다. Bearer 등 특정
헤더 형식은 기본값으로 정하지 않으며 redirect·쿠키·자동 HTTP 인증·평문 fallback은 사용하지 않는다.

Control·Telemetry JSON에 파일 바이트나 대용량 Base64를 넣지 않는다.
파일은 Host가 승인한 endpoint와 파일·세션·상한·만료에 한정된 권한으로 HTTPS PUT 전송한다.
Guest가 지정한 임의 URL이나 임의 경로를 그대로 사용하는 범용 업로드 API를 만들지 않는다.

## 7. 후보 보고와 파일 반출 — 현재 연결과 후속 흐름

```text
Runner 내부 후보 이벤트                         [현재 구현]
    ↓ CandidateTelemetry + ArtifactCandidateAdapter [제품 연결 구현, Host 합의 대기]
SECURITY_EVENT(category = ARTIFACT_CANDIDATE)
    ↓ Telemetry WSS
Host 이벤트 저장 → EVENT_ACK
    ↓ 별도의 후보 승인·전송 요청                 [이하 미구현]
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
| `candidate_id` | Issue #11 계약에서는 별도로 사용하지 않음. 후속 요청 식별은 별도 계약 |
| `event_id` | 현재 `SecurityEvent`의 ID. 후보 관찰마다 생성해 Pending·ACK·변환 계층의 중복 추적에 사용 |
| `artifact_id` / `upload_id` | Host Artifact Broker가 발급할 파일 등록·업로드 식별자 |
| `message_id` / `sequence_number` | Envelope 및 연결·송신 방향별 전송 식별. 파일 세대와 별개 |

v0.1의 7쪽에는 후보 메타데이터에 해시가 포함되어 있다. 최신 설계에서는 Runner가 SHA-256을
계산하지 않는다. Issue #11 계약안은 해시·MIME·크기 필드를 생략하며 null·빈 해시도 보내지 않는다.
Host 합의는 대기 상태로 기록한다. 기존 Envelope 필드를 다른 의미로 재사용하지 않는다.

## 9. 빌드·배포 및 문서 관리

Host의 MSYS2 UCRT64 환경에서 CMake/Ninja로 Windows EXE를 빌드한다.
C++ 표준과 도구 버전은 팀 결정에 따르며 이 문서에서 새로 고정하지 않는다.
JsonCpp는 Host 빌드 의존성이고 `jsoncpp_static`으로 연결한다. WinHTTP·BCrypt·Crypt32는
Windows 기본 DLL을 사용한다. GUI는 User32/GDI32/Ole32 및 WIC COM을 사용한다.
GUI target은 제공 모듈의 optional/filesystem에 필요한 cxx_std_17 최소 기능을 요구한다.
검증 환경 버전은 Telemetry 문서에 기록하며 팀 동결값으로 간주하지 않는다.

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

1. 구현된 artifact-candidate-v1 계약안의 Host 공유·합의를 완료하고 실제 Host 호환성을 검증한다.
2. Host 스키마 초안의 합의를 완료하고 HELLO_ACK의 Telemetry 자격을 현재 Client에 전달해 동시 기동을 연결한다.
3. 후속 후보 등록·무효화 정책을 합의한다. EVENT_ACK를 Broker 등록 완료로 해석하지 않는다.
4. 승인된 ARTIFACT_REQUEST 처리와 제한된 HTTPS 업로드를 구현한다.
5. Host Quarantine의 검사·승인·반출과 연결해 전체 흐름을 검증한다.

이 작업들은 현재 후보 감지 구현의 완료 여부와 분리해 후속 이슈로 관리한다.
