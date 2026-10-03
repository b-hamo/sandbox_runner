# Sandbox Runner 프로젝트 구조

## 현재 기준과 책임

Runner는 Windows 격리 환경에서 GUI 실행·관찰, 세션 내부 Output 감시, 후보 보고 및 Host가 승인한 파일 전송을 맡는다. 실행 파일은 `sandbox_runner.exe` 하나다. Host MCP Broker·Runtime Manager·Artifact Broker의 승인, 파일 수신·해시 계산·백신 검사·최종 반출 판단은 Host에 남는다. Runner는 SAFE나 EXPORTED를 선언하지 않는다.

현재 구현 기준은 `develop` 452e1b5의 GUI 제품 기반과 Issue #23의 `artifact-export-v1`이다. SCRP v0.1의 전체 스키마를 동결한 것은 아니다. 기존 `host-sender-fed305b`, Host6055 GUI 및 `artifact-candidate-v1` 프로파일은 유지하며 새 반출 프로파일은 양쪽에서 명시적으로 선택한다. 상세 wire 계약은 [반출 계약](artifact-export-contract.md), 제품 연결·검증은 [구현 기록](artifact-export-implementation.md)을 따른다.

## 현재 파일 구조

```text
src/
├─ main.cpp / runner_paths.h
├─ session_context.* / runner_lifecycle.*
├─ control_session.* / gui_product.*
├─ host_gui.* / host_artifact_channels.*
├─ gui_session.*
├─ candidate_telemetry.* / artifact_candidate_adapter.*
├─ artifact_export_session.*
├─ artifact/
│  ├─ output_watcher.*
│  ├─ candidate_detector.*
│  └─ file_stability.*
├─ control/
│  └─ control_types, control_util, screen_capture, input_executor, action_scheduler
├─ runtime/
│  └─ session_workspace, sandbox_runtime
├─ protocol/scrp/
│  └─ envelope, telemetry_messages, host_sender_schema, artifact_candidate, artifact_export
└─ transport/
   ├─ control/control_receiver.*
   ├─ telemetry/telemetry_client.* / pending_event_store.* / winhttp_websocket.*
   ├─ observation/https_uploader.*
   └─ artifact/https_uploader.*
```

헤더는 구현 파일과 같은 디렉터리에 둔다. `docs/contracts/artifact-export-v1/`은 Host 구현·검증용 Schema와 예제이며 제품이 읽는 설정 파일이 아니다. `build/`와 로컬 검증용 `tests/`는 Git에서 제외한다. 공유 CMake는 테스트 파일이나 타깃을 참조하지 않는다.

| 모듈 | 현재 책임과 연결점 |
| --- | --- |
| `main` | 실행 모드 하나 선택, 콘솔 stop 이벤트 소유, 초기화 오류·종료 코드 보고 |
| `session_context` / `runner_lifecycle` | 기존 Telemetry/관리 Context 검증·주입 및 독립 Telemetry READY 대기 |
| `host_gui` | 신뢰된 Host bootstrap 검증, 실제 세션 workspace, GUI·화면 HTTPS·Output watcher 및 선택적 Artifact 채널 조립 |
| `host_artifact_channels` | 인증된 HELLO_ACK 자격으로 별도 Telemetry 시작, CHANNEL_ACK 후 후보 생산자 연결, 장애·종료 수명 관리 |
| `control_session` / `gui_product` | 관리 모드와 기존 신뢰된 내부 GUI 서비스 주입 경로 |
| `gui_session` | Host 권한·capability·lease 검증, 요청을 Runtime에 전달, ACK/지연 결과·PNG 전송·비동기 종료 |
| `control` / `runtime` | 실제 Windows 캡처·입력·직렬 worker·세션 workspace. Host Sandbox Manager의 VM 생성/종료는 담당하지 않음 |
| `output_watcher` | 핸들로 고정한 Output의 재귀 Windows 변경 알림. 오류/버퍼 초과를 명시적으로 반환 |
| `candidate_detector` | bounded 이벤트·파일 상태, debounce·두 관찰값 안정화, 로컬 generation·무효화. 후보에 `FileObservation` 전달 |
| `file_stability` | 경로·reparse·ADS·하드링크·일반 파일 검증. `VerifiedFile`은 파일과 전체 디렉터리 체인 핸들을 전송 종료까지 유지 |
| `candidate_telemetry` | event_id/관찰 시각 생성·중복 억제·후보 enqueue. 선택적 Observer로 활성 후보·무효화를 export registry에 전달 |
| `artifact_candidate_adapter` | 기존 네 필드 후보 payload와 단일 STORED/REJECTED EVENT_ACK. 별도 무효화 wire 없음 |
| `artifact_export_session` | 동일 Control 세션의 event_id→관찰값 registry, 사용한 upload_id, 전송 worker 1개·완료 슬롯 1개. 현재 세대와 열린 소스를 재검증 |
| `protocol/scrp/artifact_export` | 기존 GUI Schema 합성, artifact capability·Telemetry 자격 검증, 닫힌 Artifact 요청/결과 및 Telemetry Schema |
| `transport/control` | Envelope/session/connection/sequence/replay 검증·handlers 분배. worker 결과에 correlation/sequence를 붙여 WSS 송신 |
| `transport/telemetry` | 별도 WSS·CHANNEL handshake·bounded Pending·EVENT_ACK. 독립 모드는 기존 재연결 정책 유지 |
| `transport/artifact` | bootstrap 고정 HTTPS origin·경로, 별도 Bearer token, 64 KiB streaming, 상한·전체 기한·취소·TLS·빈 201 검증 |
| `transport/observation` | 기존 8 MiB PNG 및 SHA-256 응답 업로드 계약. Artifact 전송과 별도 권한/경로 |

## 실행 모드와 Output 경로

- `--session-context <file> [--output <existing directory>]`: 기존 독립 Telemetry/후보 모드. `event_contract: artifact-candidate-v1` 선택 시 후보를 보고한다.
- `--control-context <file>`: 기존 관리 모드. GUI/Artifact 전송을 활성화하지 않는다.
- `--host-bootstrap <file> [--host-address <IPv4>]`: 기존 GUI·PNG·Output 감시. bootstrap에 `control_contract: artifact-export-v1`와 `artifact_upload`를 함께 지정하면 새 반출 경로를 활성화한다.

독립 모드의 기본 Output은 `C:\RunnerWorkspace\Output`이다. Host bootstrap 모드는 Runtime binding에 따라 다음 경로를 만들며 외부 요청의 임의 Guest 경로를 받지 않는다.

```text
C:\RunnerWorkspace\Sessions\
└─ session-<session_id>\runtime-<runtime_id>\generation-<generation>\output\
```

Output은 Guest 내부 경로다. Host에 쓰기 가능한 Output 공유 매핑을 만들지 않는다. bootstrap 인증서 PEM에서 pin을 계산하고 선택 SHA-256을 대조한다. Windows 신뢰 체인·호스트명 검증과 leaf pin을 함께 유지하며 신뢰 인증서 배치는 외부 Runtime 책임이다.

## Artifact 시작·동작·종료

1. bootstrap/profile/포트/경로/세션/자격/TLS를 검증한다. 기존 GUI file coverage를 위해 Output 알림 감시를 준비한다.
2. Runner가 Control WSS에 HELLO를 보낸다. 새 프로파일에서 실제 `artifact.export.v1` capability를 광고한다.
3. 검증한 HELLO_ACK의 GUI 권한으로 기존 Runtime을 활성화한다. Artifact가 허용되면 같은 세션 자격으로 별도 Telemetry WSS를 비동기 시작한다. Control 수신을 막지 않는다.
4. CHANNEL_ACK 후 후보 sink를 기존 watcher에 연결한다. 그 이전 변경과 기존 파일을 재조회하는 초기 스캔은 없다. Host는 Telemetry와 기존 GUI 시작 검증이 준비된 후 작업을 시작한다.
5. 안정화 후보를 송신하기 전에 내부 registry에 event_id·경로·로컬 generation·관찰값을 기록하고 네 필드 SECURITY_EVENT를 enqueue한다. enqueue 실패 시 등록을 되돌린다. ACK는 Host 이벤트 저장만 뜻한다.
6. ARTIFACT_REQUEST는 현재 후보를 참조한다. 단일 worker가 Detector의 현재 generation, identity/크기/시간, 경로 체인·일반 파일·크기 상한을 다시 검사하고 열린 핸들로 HTTPS PUT한다.
7. worker는 WSS에 직접 쓰지 않는다. Receiver가 완료를 원 connection/correlation에 ARTIFACT_RESULT로 보내며 파일 바이트·해시·안전 판정은 Control에 넣지 않는다.
8. 변경·삭제·감시 오류는 이전 후보를 무효화한다. Telemetry 장애/유실·GUI lease 만료는 신규 반출을 막고 활성 I/O를 취소하며 자동 재활성화하지 않는다.
9. TERMINATE에서는 즉시 새 요청을 차단한다. watcher sink 분리 → 후보 worker join → Artifact worker join → Telemetry stop/join 후 결과를 drain하고 GUI 종료 결과를 보낸다. Control 단절에서는 같은 자원을 정리하며 이전 결과를 새 연결에 재생하지 않는다.

정상 최종 결과 이후 WSS는 전체 1초 안에서 송신 종료·상대 close 수신·close 완료를 기다리고 핸들을 정리한다. 실제 Sandbox에서 송신 종료만 기다릴 때 재현된 최종 응답 유실을 보완했다. 종료 중 받은 메시지는 새 작업으로 분배하지 않는다.

후보/사용 ID는 각각 최대 4096개이며 업로드 1개·완료 슬롯 1개만 둔다. 파일 바이트 전체나 임시 복사본은 Runner에 보관하지 않는다. HTTPS 201은 Host 수신 완료이며 최종 반출 성공과 별개다. [전체 제한·오류 코드](artifact-export-contract.md)를 따른다.

## Host 연결과 남은 작업

Host는 Telemetry 인증/라우터/ACK, event_id 중복 등록·artifact_id 매핑, artifact_list/export backend·승인, 한정된 업로드 grant, 전용 HTTPS 수신·수신 크기/SHA-256·결과 교차 확인, 형식/백신 검사 및 동일 바이트 최종 EXPORTED를 구현해야 한다. GUI Startup Verification과 MCP 기존 REQUIRE_APPROVAL을 그대로 연결한다. Host 구현 체크리스트는 [반출 계약 11절](artifact-export-contract.md#11-mcp-담당-구현-체크리스트)에 있다.

현재 제품 Control은 자동 재연결하지 않는다. Telemetry 독립 모드의 재연결과 새 통합 프로파일의 장애 시 중단 정책을 구분한다. 실제 Sandbox의 Runner 반출은 독립 peer로 검증했다. 실제 Host/MCP 전체 반출, Scanner 정책·Host 채택 revision은 후속 통합 검증 대상이다.

## 빌드·검증 문서

Host의 동일 MSYS2 UCRT64 환경에서 CMake/Ninja로 Windows x64 EXE를 빌드한다. 기존 GUI 기반의 C++17 요구와 JsonCpp 정적 연결을 유지한다. 도구/라이브러리 버전은 검증 환경이며 팀 동결값으로 새로 확정하지 않는다. Sandbox에는 실행 결과만 배치한다.

[README](../README.md), [Telemetry](telemetry.md), [후보 감지](artifact-candidates.md), [GUI 연동](gui-integration.md), [기존 Host 관찰 검증](host-observation-integration.md), [Artifact 구현·검증](artifact-export-implementation.md), [실제 Sandbox Artifact 검증](artifact-sandbox-validation.md)을 따른다.
