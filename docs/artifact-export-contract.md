# 파일 반출 구현 계약 — artifact-export-v1

## 1. 상태와 적용 범위

- 작성일: 2026-10-03.
- 최신 Host 비교 기준: 사용자 지정 `feat/24-sandbox-launch`, commit `0690845ef20c272ed45ed83a9415a73edcb71365`.
- 상태: **Issue #23에서 Runner 제품 구현. Host/MCP 채택·실제 상호 운용 검증 대기.**
- 목적: 중간 발표에서 실행 중 생성한 단일 파일을 후보 보고 → Host 요청 → HTTPS 수신 → 검사 → 최종 반출하는 데 필요한 계약을 제공한다.
- 사용자 결정: 이미 맞춘 Host 계약은 유지하고, 비어 있거나 불일치하는 Artifact 계약은 Runner 측에서 정의해 Host/MCP 담당자의 구현 문서로 제공한다.
- 이 문서의 프로파일명·필드·상한은 **Runner가 구현한 명시적 v1 계약**이다. 기존 SCRP 전체의 동결값으로 해석하지 않는다. Host도 이 프로파일을 구현하고 선택해야 한다.
- 기존 `host-sender-fed305b`와 `artifact-candidate-v1`의 의미·제품 동작은 변경하지 않는다. 새 프로파일을 양쪽에서 구현하고 명시적으로 선택해야 한다.
- C++ 후보 레지스트리·HTTPS 업로더·Control/Telemetry 동시 lifecycle을 구현했다. Host 서버와 MCP 도구는 수정하지 않았다. 13~14절은 구현 전 문서 검증 이력이다.

검증 가능한 자료:

| 자료 | 확인 결과 |
| --- | --- |
| Runner `develop` 452e1b5 기반 Issue #23 | 기존 GUI·Output·PNG 업로드에 후보 보고, event_id 레지스트리, HTTPS Artifact 업로드를 통합. 원 a021632 작업 트리의 미커밋 변경은 보존 |
| [Runner #11 완료 기록](https://github.com/b-hamo/sandbox_runner/issues/11#issuecomment-5856376530) | 후보 계약 구현 완료, Host 합의·업로드·반출은 후속 |
| [Runner #13](https://github.com/b-hamo/sandbox_runner/issues/13) | Host fed305b 호환 검증. #11 Artifact 계약은 별도 유지 |
| [최신 Host feat/24-sandbox-launch](https://github.com/b-hamo/host_control/tree/0690845ef20c272ed45ed83a9415a73edcb71365) | WSS 인증·MCP/Broker·Sandbox lifecycle·스크린샷 HTTPS 업로드 구현. Artifact와 Telemetry의 실제 수신·반출은 후속 |
| [이전 Host fed305b의 schema 기준](https://github.com/b-hamo/host_control/blob/fed305b06dfcfdced406c316514cd3fd2e03a911/schema/README.md) | 기존 로컬 Runner 호환 검증 이력. 최신 Host 기준으로 사용하지 않음 |

초판은 오래된 `feat/1-host-sender`만 확인했다. 사용자 지정 최신 브랜치를 추가 비교하여 아래
Host 현황·포트·bootstrap·MCP 이름·승인·READY 조건을 정정했다. 기존 #13 테스트 결과는 당시 이력으로 유지한다.

첨부:

- [메시지 JSON Schema](contracts/artifact-export-v1/messages.schema.json): 이 프로파일의 Artifact/Telemetry 메시지 6종.
- [전체 Envelope 예제](contracts/artifact-export-v1/examples.json): 성공 흐름과 실패 결과.
- [기존 후보 계약](artifact-candidate-contract.md): 후보 payload·EVENT_ACK의 기존 의미.
- [기존 Host 연동 기록](host-sender-integration.md): 변경 없이 유지할 fed305b 호환 프로파일.

## 2. 유지할 계약과 해결할 차이

| 항목 | 확인한 차이 | 이번 제안 |
| --- | --- | --- |
| Control 기본 연결 | fed305b와 관리 요청·Envelope 호환 검증됨 | 기본 16필드 Envelope, WSS, 관리 요청 유지 |
| 후보 payload | Host 초안은 process/evidence 및 크기·해시 요구, Runner #11은 4필드 | #11의 `event_id/observed_at/category/relative_path` 사용 |
| 후보 요청 식별 | Host는 별도 `candidate_id`, #11에는 해당 필드 없음 | 요청에 `candidate_event_id`를 두고 보고된 `event_id`를 직접 참조 |
| EVENT_ACK | Host는 `event_ids[]`와 OK/ERROR, Runner는 단일 `event_id`와 STORED/REJECTED | #11 단일 ACK 유지. Host의 payload 및 Envelope status/error 검증을 프로파일별 분기 |
| 업로드 권한 | Host 요청 초안에 파일별 토큰 전달 경로 없음 | 요청에 `upload_token` 추가. 전용 HTTPS PUT에서만 사용 |
| ARTIFACT_RESULT | Host 초안은 Runner sha256 필드 요구 | Runner 해시 필드 생략. 실제 수신 크기·SHA-256은 Host 계산 |
| Telemetry 연결 | 제품은 외부 Context, Control과 별도 실행 | 검증된 HELLO_ACK의 Telemetry 자격을 같은 세션의 Client에 전달 |
| 업로드 완료 응답 | 현재 동기 ReplyHandlers, 제품 성공 스키마 없음 | 단일 업로드 worker + 제한된 완료 큐. 파일 I/O로 Control 수신을 막지 않음 |
| 반출 | 업로드 성공과 최종 반출은 서로 다름 | HTTP 수신 완료 + Runner 결과 일치 + Host 검사·정책 통과 후만 공개 |

### 최신 Host에 이미 있는 기반과 추가 연결 조건

| 항목 | Host 0690845의 실제 상태 | 이 계약에 적용할 내용 |
| --- | --- | --- |
| WSS·세션 인증 | `sender.py`가 TLS·Authorization 검사, 일회용 token 소비, HELLO 세션 대조 수행 | 신규 개발 항목에서 제외하고 재사용. Telemetry/Artifact의 별도 권한 검증은 추가 |
| HTTPS 서버 | `upload_server.py`는 기본 17444의 스크린샷 전용 수신기. Control은 기본 17443 | Control과 같은 포트라는 초판 전제를 폐기. bootstrap으로 Artifact 업로드 포트를 따로 지정 |
| 기존 업로드 동작 | PNG, 8 MiB, body 전체 메모리 수신, upload_id 자체가 capability, 201 body는 SHA-256 | 일반 파일용 디스크 스트리밍·별도 토큰·빈 201 응답은 새 Artifact 경로의 계약. 스크린샷 경로를 그대로 재사용하지 않음 |
| Telemetry | HELLO_ACK에 무작위 자격은 만들지만 등록/인증/수신 경로 없음. Upgrade 라우터는 Control 경로만 허용 | token 발급을 실제 세션 registry에 연결하고 Telemetry 라우터·ACK 처리 추가 |
| MCP | `artifact_list`, `artifact_export` 입력 스키마가 이미 있음. `NOT_YET_AVAILABLE`로 숨김 | 이름과 `artifact_id` 입력을 유지하고 backend·도구 가용성·상태 연결 추가 |
| 승인 | `Policy`는 artifact_export에 REQUIRE_APPROVAL, Broker에는 approver hook. MCP의 Broker 생성은 approver 미지정 | 도구 노출만 풀면 동작하지 않음. Host 승인 경로를 연결하고 검사 통과와 사용자 승인을 구분 |
| READY | 기본 StartupProfile은 gui.observe/gui.input, file monitoring, worker_alive, 첫 캡처·heartbeat 확인 | Artifact capability 하나만 추가해도 READY가 되지 않음. 실제 GUI 지원 Runner와 통합하고 기존 시작 검증 유지 |
| Bootstrap | `host/port/path/token/host_certificate_pem/host_certificate_sha256/observation_upload` 형식 | 최신 Runner의 `--host-bootstrap`에서 직접 지원. v1 선택·Artifact 설정 두 필드를 추가 |
| TLS 신뢰 | Host README는 bootstrap 인증서 pinning을 안내하며 현재 주소를 SAN에 포함. 이 로컬 Runner는 Windows chain/hostname + 선택적 pin | 두 설정을 같은 것으로 취급하지 않음. 신뢰 전달 방식은 통합 시 확인하고 인증서 검증을 임의로 끄지 않음 |
| Lifecycle | `task_submit` 백그라운드 시작, READY 후 정리, 세션 종료 stop/cleanup 구현 | 새 Sandbox Manager를 만들지 않고 종료 시 Artifact 취소·정리 hook을 연결 |

근거: [sender.py](https://github.com/b-hamo/host_control/blob/0690845ef20c272ed45ed83a9415a73edcb71365/host/sender.py),
[upload_server.py](https://github.com/b-hamo/host_control/blob/0690845ef20c272ed45ed83a9415a73edcb71365/host/upload_server.py),
[bootstrap.py](https://github.com/b-hamo/host_control/blob/0690845ef20c272ed45ed83a9415a73edcb71365/host/bootstrap.py),
[tool_availability.py](https://github.com/b-hamo/host_control/blob/0690845ef20c272ed45ed83a9415a73edcb71365/host/tool_availability.py),
[mcp_server.py](https://github.com/b-hamo/host_control/blob/0690845ef20c272ed45ed83a9415a73edcb71365/host/mcp_server.py),
[startup.py](https://github.com/b-hamo/host_control/blob/0690845ef20c272ed45ed83a9415a73edcb71365/host/startup.py).

[Host #24](https://github.com/b-hamo/host_control/issues/24)에는 별도의 Runner develop `452e1b5`로 실기 시험을
통과했다는 담당자 기록이 있다. 이 로컬 관리 모드에서 최신 Host의 READY가 통과한다는 증거로 사용하지 않는다.
담당자의 실기 보고와 이번 정적 비교를 구분한다.

Host의 기존 SECURITY_EVENT에 요구되는 값을 임의로 0/null/가짜 해시로 채워 호환시키지 않는다.
다른 보안 이벤트 category의 계약은 이 문서가 변경하지 않는다.

## 3. 최소 범위와 역할

발표 기본 정책은 **실행 후 생성된 UTF-8 TXT 일반 파일 한 개**다. 다른 형식은 검사 정책을 추가하기 전 자동 반출하지 않는다.
형식 허용은 안전성 보장이 아니며 파일 자동 실행·미리보기는 하지 않는다.

| 구성요소 | 책임 |
| --- | --- |
| Runner | 내부 Output 감시·후보 기록, 요청 시 현재 후보/파일 재검증, 단일 파일 스트리밍, 전송 결과 보고 |
| Host Artifact Broker | 후보 등록, Host 정책에 따른 승인, 업로드 권한·상태 관리, 검사 조율, 최종 반출 |
| Host 수신기 | 인증·용량·기한 검증, 비공개 임시 저장, 수신 크기·해시 계산, 수신 완료 고정 |
| Host 검사 모듈 | 지원 형식 확인·기존 백신 검사, 파일과 연결된 결과 반환. 오류/미설치/시간 초과는 통과 아님 |
| Sandbox Manager | 세션·Runtime 생성/종료, 신뢰 설정·endpoint 전달, 종료 통지. 파일 안전 판정은 하지 않음 |
| MCP 담당 | 등록된 후보 조회, 반출 요청, 상태·실제로 접근 가능한 최종 결과 참조 제공 |

DB, 추가 검사 VM, 동적 분석, 압축/폴더 일괄 전송, 이어받기, 자동 재전송, 병렬 업로드는 제외한다.
Host 후보·전송 상태는 메모리에서 관리할 수 있다. `STORED`는 해당 활성 세션 저장소가 이벤트를
수용했다는 의미이며 디스크 영속성이나 재시작 복구를 보장하지 않는다. Host 재시작 시 이전
세션/Runtime generation의 인증과 업로드 권한을 폐기하고 새 문맥으로 시작한다.
기존 다른 프로파일의 영속 저장 약속을 이 규칙으로 변경하지 않는다.

## 4. 명시적 프로파일 선택과 시작

후속 구현은 Host 세션 설정과 Runner Context에 `artifact-export-v1`을 명시적으로 선택한다.
Runner의 선택값은 Host bootstrap의 `control_contract: "artifact-export-v1"`다. **`--host-bootstrap` 경로에서만 지원한다.**
`--control-context`는 기존 관리 모드이며 반출을 활성화하지 않는다.
현재의 `event_contract: "artifact-candidate-v1"` 단독 설정으로는 반출이 활성화되지 않는다.

1. Runtime이 신뢰할 Host WSS endpoint, 세션 식별자, bootstrap 자격, TLS 신뢰를 전달한다.
2. Runner는 기존 HELLO 구조의 `capabilities`에 구현 완료된 `artifact.export.v1`을 광고한다.
3. Host는 이 프로파일로 구성된 세션에서만 HELLO_ACK의 `allowed_capabilities`에 같은 값을 허용한다.
   해당 값이 없거나 프로파일이 다르면 Artifact 경로를 활성화하지 않는다. 구형 계약으로 자동 전환하지 않는다.
4. HELLO_ACK의 기존 `channel_credentials.telemetry`의 `token/expires_at`을 검증해
   `Authorization: Bearer <token>`으로 Telemetry WSS에 연결한다.
5. 기존 GUI의 file coverage 확인을 위한 Output 알림 감시는 HELLO 전에 시작한다. CHANNEL_ACK 확인 후
   같은 감시에 후보 생성·보고를 연결한다. 그 이전 변경과 기존 파일을 재조회하는 초기 스캔은 없다.
   Host는 Telemetry 연결·후보 등록과 기존 GUI Startup Verification을 기다려 작업을 시작해야 한다.
6. GUI 지원 Runner와 통합되어 기존 GUI capability/worker/관찰 업로드/heartbeat 경로를 유지한다.
   Host는 기존 Startup Verification으로 READY가 된 뒤에만 반출 요청을 발행한다. Runner는 GUI lease·감시와
   Telemetry 상태를 확인하지만 새 READY wire 메시지나 자체 검사 승인을 만들지 않는다.

HELLO/HELLO_ACK의 필드는 추가하지 않는다. Host가 가진 Capability enum에 `artifact.export.v1`을
추가하고 `sender.py`의 `ALLOWED_CAPABILITIES`에도 연결해야 한다. 스키마만 바꾸면 Host가 새 capability를
허용 목록에서 제거하므로 활성화되지 않는다. 광고한 값의 부분집합만 허용하고 세션 문맥으로 프로파일을 고정한다.
기본 HELLO_ACK의 limits/reconnect 자격 구조는 기존 Control 계약을 따른다.
Telemetry 장애가 나면 신규 반출 요청을 중지하고 활성 전송도 실패 처리한다.
인증·세션 binding 없이 Guest가 보내는 session_id만 믿지 않는다.

Endpoint의 호스트는 신뢰할 bootstrap으로 고정하되, 포트는 채널별 bootstrap 설정을 사용한다.
최신 Host는 Control과 별도 HTTPS 서버를 사용한다. Artifact 수신은 기존 HTTPS 서버에 전용 경로를
추가하는 방향으로 제안하며, **Control과 동일 포트를 강제하지 않는다.**

| 채널 | 설정과 경로 |
| --- | --- |
| Control WSS | 현재 bootstrap `port/path`, 기본 17443 / `/scrp/v1/control` |
| Telemetry WSS | 후속 구현에서는 Control 포트에 `/scrp/v1/telemetry` 라우터 추가 |
| Observation HTTPS PUT | 현재 `observation_upload.port/path`, 기본 17444 / `/scrp/v1/observations/` |
| Artifact HTTPS PUT | 새 제안 `artifact_upload.port/path`, 예시 17444 / `/scrp/v1/artifacts/` + upload_id |

다음은 기존 Host bootstrap에 **추가할 필드만** 발췌한 설정이다. Runner가 지원하며 Host writer 연결이 필요하다.

~~~json
{
  "control_contract": "artifact-export-v1",
  "artifact_upload": {
    "port": 17444,
    "path": "/scrp/v1/artifacts/"
  }
}
~~~

포트는 1..65535 정수, path는 이 프로파일에서 `/scrp/v1/artifacts/`로 고정한다.
Host의 실제 바인딩 포트를 기록하며, 값이 없거나 잘못되면 Artifact 전송을 활성화하지 않는다.
Observation 설정을 Artifact 권한으로 자동 재사용하거나 17443/17444를 추측해 접속하지 않는다.
업로드 요청에는 URL·Host 저장 경로·Guest 경로를 넣지 않는다. 인증서 체인·호스트명 검증을 유지하며
redirect, 평문 fallback, 임의 proxy/목적지 변경, 자동 HTTP 인증은 허용하지 않는다.
최신 Host의 WSS/HTTPS·Control token 검증은 이미 있다. Artifact 및 Telemetry 권한은 추가해야 한다.
Host bootstrap PEM/pin을 이 로컬 Runner의 Windows 신뢰 설정에 연결하는 작업은 별도 통합 조건이다.
Host README의 pinning 방식과 현재 로컬 chain/hostname 검증을 자동 호환으로 간주하지 않는다.
검증 방식을 바꾸려면 공용 Transport 계약과 함께 별도로 구현·검증한다. 이 문서는 검증 해제를 승인하지 않는다.

## 5. Envelope·Telemetry 규칙

기존 16필드 Envelope와 `version: "1.0"`을 사용한다. 연결·방향별 sequence는 1부터 시작한다.
message_id/nonce는 송신마다 새로 생성하고 세션·Runtime generation·connection_id·시각·재전송을 검증한다.
요청/이벤트의 correlation_id/status/error는 null, Artifact 및 이 Telemetry 흐름의 task_id/action_id는 null이다.
응답 correlation_id는 원래 요청/이벤트의 message_id다. Control과 Telemetry는 각자의 connection_id를 갖는다.

이 프로파일은 1회성 ACK를 Artifact 전송 시작 시 보내지 않는다. 최종 `ARTIFACT_RESULT` 하나만 보낸다.
수신한 ARTIFACT_REQUEST는 worker로 넘기고 즉시 Control 수신을 계속한다.

| 메시지 | payload 및 추가 규칙 |
| --- | --- |
| CHANNEL_HELLO | `{"channel":"telemetry"}`, sequence=1, connection_id=null |
| CHANNEL_ACK | `{"channel":"telemetry","max_event_bytes":16384}`, sequence=1, status=OK, error=null, 새 connection_id |
| SECURITY_EVENT | 아래 4필드. size/hash/MIME 및 별도 candidate_id 없음 |
| EVENT_ACK 성공 | `{"event_id":"<원래 event_id>"}`, status=STORED, error=null |
| EVENT_ACK 거부 | 같은 payload, status=REJECTED, error는 `{"code":"STORAGE_REJECTED"}`만 허용 |

~~~json
{
  "event_id": "b64fbf52-4f4e-4c99-994c-e7f0e8d29101",
  "observed_at": "2026-10-03T03:00:00.123Z",
  "category": "ARTIFACT_CANDIDATE",
  "relative_path": "reports\\결과.txt"
}
~~~

기존 후보 payload 생성은 밀리초 3자리다. 실제 현재 파서와 이 프로파일은 초 또는 소수점 1~6자리 UTC Z 시각을 수용한다.
relative_path는 #11의 Windows 상대 경로 규칙을 따른다. 이 값은 표시용 비신뢰 메타데이터이며 Host 저장 경로가 아니다.
Host는 먼저 이벤트를 저장하고 ACK한다. Broker가 후보를 등록한 뒤에만 조회/반출 요청 대상으로 노출한다.
EVENT_ACK만으로 Broker 등록·업로드·검사·반출을 완료 처리하지 않는다.
동일 event_id·동일 payload 재전송은 저장을 중복하지 않고 ACK한다. 동일 ID에 다른 payload는 거부한다.

Control 메시지 상한은 협상값(최대 64 KiB), Telemetry는 Envelope 포함 16 KiB다.
이 최소 프로파일의 CHANNEL_ACK는 `max_event_bytes=16384`만 지원한다. 크기는 직렬화한 UTF-8 바이트로 계산한다.
JSON 중복 키·NaN/Infinity·잘못된 UTF-8·과도한 중첩(16 초과)을 거부한다.

## 6. 후보 식별과 변경

후보 키는 `(session_id, runtime_id, generation, event_id)`다.
`candidate_event_id`는 이 키의 event_id를 참조하는 필드이며, 새 식별자를 생성하지 않는다.
기존 Host MCP 입력에 맞춰 Host가 등록 시 `artifact_id`를 발급하고 위 후보 키에 매핑한다.
Agent는 artifact_id로 선택하며 Runner에 보내는 것은 매핑한 candidate_event_id다.
artifact_id를 Guest 경로로 사용하거나 다른 세션의 후보를 연결하지 않는다.

Runner는 향후 조회 가능한 후보 레지스트리에 상대 경로·로컬 파일 세대·안정화 관찰값을 기록한다.
현재 CandidateTelemetry의 private map은 전송용 조회 API가 아니므로 새 연결이 필요하다.
파일 재수정 후 안정화되면 새 event_id를 사용한다. 기존 event_id를 새 파일에 재지정하지 않는다.

v1은 별도 무효화 wire 메시지를 추가하지 않는다. 따라서 Host 목록은 **보고된 후보 목록**이며
Output의 완전하고 항상 최신인 파일 목록이 아니다. 과거 후보는 요청 시 Runner가 거부할 수 있다.
관찰 시각만으로 후보 순서를 추론하거나 새 이벤트를 과거 후보의 무조건적 대체로 간주하지 않는다.

요청 시 Runner가 확인할 사항:

- 같은 세션의 활성 후보인지, 감시 상태가 정상인지, 로컬 변경 세대가 같은지 확인한다.
- Output 하위 경로를 핸들 기반으로 다시 열고 reparse point·하드링크·ADS·비일반 파일을 거부한다.
- 기록한 관찰값과 현재 파일 identity/크기/수정 정보를 비교한다. 실패하면 CANDIDATE_UNAVAILABLE로 종료한다.
- 검증한 파일과 경로 체인 핸들을 전송 종료까지 유지하고, 검증 후 경로로 재오픈하지 않는다.
- 파일의 쓰기/삭제 공유를 제한한다. 잠글 수 없거나 변경이 발견되면 새 후보를 기다리도록 실패 처리한다.

후보는 관찰 기록이며 암호학적으로 고정한 파일 스냅샷이 아니다. 위 비교만으로 악의적인 Guest의
내용 변경을 완전히 증명할 수 없다. Host는 최종 수신한 바이트를 비신뢰 입력으로 검사한다.
observe_file()은 관찰 후 핸들을 닫는다. 업로드는 별도 `open_verified_file()`로 열고
`VerifiedFile`이 파일과 드라이브/디렉터리 체인 핸들을 전송 종료까지 보유한다.

## 7. ARTIFACT_REQUEST

Host는 승인한 후보에 대해 **새 upload_id와 토큰**을 먼저 메모리에 등록한 뒤 Control로 요청한다.

~~~json
{
  "candidate_event_id": "b64fbf52-4f4e-4c99-994c-e7f0e8d29101",
  "upload_id": "u_MidtermDemo_20261003_01",
  "upload_token": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
  "upload_deadline_at": "2026-10-03T03:02:01Z",
  "max_bytes": 1048576
}
~~~

위 토큰은 문서 예시이며 실제 권한으로 사용하지 않는다.

| 필드 | v1 제안 |
| --- | --- |
| candidate_event_id | 소문자 UUIDv4, 현재 세션에서 보고된 후보 event_id |
| upload_id | `[A-Za-z0-9_-]{16,128}`. Host가 암호학적 난수로 생성, 최소 128비트 엔트로피 |
| upload_token | 32바이트 암호학적 난수의 padding 없는 base64url 43자 |
| upload_deadline_at | UTC Z 시각. 수신 완료까지의 기한. 예시 기본 요청 수명 120초 |
| max_bytes | 정수 1..52,428,800. Host 정책으로 더 낮게 설정 가능; 예제는 1 MiB |

50 MiB 상한은 기존 Host 초안과 같은 값을 채택한 **이 v1 제안의 상한**이다.
권한은 session/runtime/generation/candidate_event_id/upload_id/PUT 경로/상한/만료에 묶는다.
upload_id는 공개 식별자이며 인증 수단이 아니다. 인증은 토큰과 Host의 활성 grant 상태로 검증한다.
토큰을 URL·로그·MCP 응답에 노출하지 않는다. 원문 요청 로그도 redaction한다.
검증된 deadline은 전송 전체 제한에 적용하고 시계 변경으로 기한이 연장되지 않도록 monotonic budget도 적용한다.

## 8. HTTPS PUT·불완전 전송

~~~http
PUT /scrp/v1/artifacts/u_MidtermDemo_20261003_01 HTTP/1.1
Host: host.example:17444
Authorization: Bearer AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
Content-Type: application/octet-stream
Content-Length: 6

hello
~~~

실제 예제 body는 UTF-8 `hello\n`의 6바이트다. 헤더의 원본 파일명·상대 경로는 전송하지 않는다.
압축·multipart·Content-Range·Transfer-Encoding은 지원하지 않는다. Content-Encoding은 생략한다.
Runner는 열린 핸들의 크기를 Content-Length로 사용해 고정 크기 버퍼로 읽는다. 빈 파일은 0바이트로 허용한다.

Host 수신 동작:

1. body 수신 전에 인증·세션·활성 grant·기한·Content-Length·미사용 upload_id를 확인한다.
2. grant를 원자적으로 CLAIMED로 전환한다. 같은 upload_id의 동시/후속 PUT은 받지 않는다.
3. 전용 저권한 저장소에 Host가 생성한 이름으로 새 임시 파일을 만든다. 기존 파일을 덮어쓰지 않는다.
4. 실제 읽은 바이트도 별도로 세어 선언 길이·상한·기한·전체 저장소 한도를 강제한다.
5. 길이가 정확하고 쓰기/닫기가 성공하면 파일을 변경 불가능한 검사 대기 객체로 확정하고 크기·SHA-256을 기록한다.
6. 그 뒤 `201 Created`, 빈 body로 응답한다. 이는 수신 성공이며 검사·반출 성공은 아니다.

| HTTP 결과 | 의미 |
| --- | --- |
| 201 | 새로운 파일의 수신 완료. Runner가 UPLOADED 결과를 보낼 조건 |
| 401 / 403 | 누락/잘못된 권한 또는 scope 불일치. 대상 정보는 응답으로 노출하지 않음 |
| 409 | 이미 사용 중이거나 소비된 upload_id. 기존 파일을 변경하지 않음 |
| 410 | 유효한 요청 문맥에서 확인된 만료/취소 grant |
| 411 / 413 / 415 | 길이 누락 / 용량 초과 / 지원하지 않는 전송 표현 |
| 400 / 408 / 5xx | 잘못된 전송 / 시간 초과 / 저장 등 내부 실패 |

Runner는 **201만** 수신 완료로 처리한다. 3xx·다른 2xx·연결 끊김을 성공으로 해석하거나
같은 ID로 자동 재전송하지 않는다. 오류 응답의 자유 텍스트를 로그에 복사하지 않는다.
인증되지 않은 요청은 정상 grant를 소비하거나 다른 업로드를 취소할 수 없다.
409 응답도 이미 진행 중인 정상 업로드의 파일·상태를 변경하지 않는다.
인증을 통과해 CLAIMED가 된 전송이 실패하면 부분 파일은 최종 결과에서 제외하고 grant는 재사용하지 않는다.
정상 수신 후 응답이 유실된 경우도 Host에 파일이 있을 수 있으므로 즉시 재전송하지 않는다.
다시 시도하려면 Host가 후보 상태를 확인하고 새 grant/새 request를 발급한다.

## 9. ARTIFACT_RESULT와 상관관계

결과는 원래 Control 요청과 같은 connection_id에서 보내고 correlation_id는 해당 요청의 message_id다.
sequence는 결과 전송 시점의 Control 송신 순서로 부여한다. worker가 직접 WebSocket에 쓰지 않는다.

성공:

~~~json
{
  "candidate_event_id": "b64fbf52-4f4e-4c99-994c-e7f0e8d29101",
  "upload_id": "u_MidtermDemo_20261003_01",
  "transfer": "UPLOADED",
  "bytes_sent": 6
}
~~~

status=OK, error=null이며 성공은 정상 201 수신 후만 보고한다. `sha256`·`SAFE`·최종 경로는 보내지 않는다.

실패 payload는 같은 4필드에서 transfer=FAILED 또는 EXPIRED, bytes_sent는 HTTP 송신기에
전달한 파일 body 바이트 수(전송 전 실패면 0)다. 이 값은 Host의 실제 수신 크기가 아니다.
status=ERROR이며 error는 아래 닫힌 object다.

~~~json
{
  "code": "CANDIDATE_UNAVAILABLE",
  "message": "Candidate unavailable",
  "retryable": false,
  "recommended_next_step": null
}
~~~

| code | transfer | 의미 |
| --- | --- | --- |
| CANDIDATE_UNAVAILABLE | FAILED | 후보 없음·무효화·삭제·변경·소스 잠금 실패 |
| ARTIFACT_BUSY | FAILED | 이미 업로드 중. 추가 작업 큐는 두지 않음 |
| ARTIFACT_BLOCKED | FAILED | 허용 경로/일반 파일 조건/크기 정책 불충족 |
| UPLOAD_FAILED | FAILED | HTTP·TLS·읽기·통신 실패 |
| UPLOAD_EXPIRED | EXPIRED | 전송 기한 만료 |
| SESSION_TERMINATED | FAILED | Host 종료 요청 또는 세션 무효화 |
| INTERNAL | FAILED | 나머지 내부 실패, 안전한 고정 진단만 노출 |

이 code들은 새 프로파일의 Artifact 결과에 한해 추가한다. 기존 공통 ERROR code 집합을
모든 메시지에 걸쳐 무제한 확장하지 않는다. message는 구현이 정한 고정 진단(최대 256자)이다.
retryable=false는 **같은 요청·grant를 자동 재실행하지 말라**는 뜻이다. 새로운 Host 승인 요청을 막지 않는다.
잘못된 JSON/스키마/세션 요청은 worker로 전달하지 않고 기존 프로토콜 거부 정책을 따른다.

## 10. Host 검사·최종 반출과 종료

Host는 다음 두 조건을 같은 grant에 연결해 확인한 뒤 검사한다.

- 수신기가 해당 upload_id를 완료했고 수신 파일을 고정했다.
- 원래 Control 요청에 대한 UPLOADED 결과를 받았으며 후보 ID·upload_id·bytes_sent가 일치한다.

Runner 결과만 먼저 오거나 실제 파일이 없으면 검사/반출하지 않는다. HTTP 201만 있어도 공개하지 않는다.
Host가 검사 결과를 파일 객체·Host 계산 해시·검사 정책에 연결하고, 검사 후에도 같은 바이트만 공개한다.
백신 API가 수정/치료한 파일은 원래 파일의 검사 성공으로 취급하지 않는다. 최소 구현은 반출 보류한다.
백신 정상 종료 코드만으로 미탐지를 추정하지 말고 사용한 엔진의 탐지/치료/실패 의미를 확인한다.

Host 내부 상태 예시는 다음과 같다. SCRP에 새 최상위 메시지를 추가하는 상태 이름이 아니다.

~~~text
CANDIDATE → AUTHORIZED → RECEIVING → RECEIVED → SCANNING → EXPORTED
                         실패/만료/차단 → FAILED / EXPIRED / BLOCKED
~~~

수신 후 결과 대기에도 deadline을 둔다. upload_deadline_at까지 수신을 완료하고
그 뒤 최대 10초 안에 유효한 ARTIFACT_RESULT가 없으면 FAILED로 처리해 공개하지 않는다.
검사 시간 제한은 별도 Host 정책이며 예시 기본값은 30초다. 이 시간들은 v1 제안/설정값이다.
검사 중에도 Control heartbeat와 종료 요청을 처리한다.

Control 단절, TERMINATE, Runtime generation 변경 시 **미반출 작업을 취소**하고 grant를 폐기한다.
최신 develop 기반 제품 Control은 자동 재연결하지 않는다. 새 실행은 새 세션/자격으로 시작한다.
이전 connection_id의 결과를 새 연결에 재생해 완료 처리하지 않는다.
Telemetry 장애·감시 오류·GUI lease 만료 뒤 같은 실행의 반출 경로는 자동 재활성화하지 않는다.
Host가 취소를 관찰하기 전에 이미 EXPORTED로 확정한 결과는 중복 공개/덮어쓰기하지 않는다.
최종 publish와 취소는 같은 상태 잠금/원자적 전이로 직렬화한다.
반출 전에 Scanner/Receiver가 파일을 사용 중이면 작업을 중지하고 핸들이 닫힌 후 정리한다.

## 11. MCP 담당 구현 체크리스트

최신 Host에 이미 있는 `artifact_list`, `artifact_export` 이름과 입력 스키마를 재사용한다.
Agent가 session_id나 Guest 경로를 지정하지 않고 Broker의 현재 세션에 묶이는 규칙도 유지한다.
별도 상태 조회 도구를 새로 만들지 않고 artifact_list의 반환 상태를 사용한다.

| 기능 | 입력/반환 및 제약 |
| --- | --- |
| `artifact_list` | 기존 status/limit/cursor 입력 유지. Host artifact_id·표시 경로·상태 제공. 수신 전 크기·MIME·해시는 미확인으로 표현 |
| `artifact_export` | 기존 artifact_id/reason 입력 유지. Host 승인 후 대응 후보의 전송·검사·반출 조율. 토큰은 Agent에 반환하지 않음 |
| 상태 조회 | artifact_export는 수락 후 artifact_id·현재 상태를 반환하고 진행은 artifact_list로 확인. Runtime 세대 경계 유지 |
| 결과 제공 | EXPORTED 이후에만 실제 접근 가능한 최종 파일 참조 제공. 임시 경로는 숨김 |

**현재 Host 도구 설명과 남은 의미 차이:** 기존 artifact_export 설명은 이미 SAFE인 파일만 받는다.
이 v1 제안은 PENDING 후보의 전송·검사까지 시작하는 의미다. 입력 필드는 같아도 의미는 달라지므로
채택 시 도구 설명·반환 상태·정책을 함께 맞춰야 한다. 기존 SAFE-only 동작을 유지한다면 별도의
Host 승인된 수집·검사 단계가 먼저 있어야 하며, 업로드 없이 SAFE로 표시해 우회해서는 안 된다.

기존 artifact_list 필터(enum ANY/PENDING/SCANNING/SAFE/BLOCKED/QUARANTINED/EXPORTED)는 유지한다.
내부 전송 상태 CANDIDATE/AUTHORIZED/RECEIVING은 PENDING, 수신 후 검사 대기는 QUARANTINED로
매핑하는 방식으로 연결한다. 검사 실패·오류·전송 실패 원인은 별도 결과 정보로 제공하고,
SAFE는 Host 정책 검사를 통과한 상태라는 기존 API 이름일 뿐 절대 안전 보장이 아니다.
같은 artifact_id의 중복 호출은 진행 중/완료 상태를 반환해 기존 idempotentHint를 지킨다.
실패 후 새 grant 발급은 실패 이유·현재 후보·Host 승인을 다시 확인한 명시적인 새 시도로만 한다.

- [x] 기존 WSS/HTTPS 서버, Control 인증·세션 대조, MCP/Broker, Sandbox lifecycle 기반 확인(소스 기준).
- [ ] 최신 bootstrap → Runner 설정/TLS 신뢰 연결, 실제 READY 및 Artifact 선택 확인.
- [ ] 새 프로파일 선택과 capability enum 확장. 구형 Host·Runner의 오접속 거부.
- [ ] SECURITY_EVENT payload, EVENT_ACK payload/status/error의 프로파일별 validation.
- [ ] Telemetry 라우터와 세션별 token 등록·인증, CHANNEL_HELLO/ACK 및 HELLO_ACK 자격의 binding.
- [ ] 후보 event_id 중복 저장 방지와 Broker 등록. 오류/재시작 시 이전 세션 무효화.
- [ ] bootstrap artifact_upload 전달, 기존 HTTPS 서버의 별도 Artifact 경로, 제한된 grant·디스크 스트리밍·부분 파일 정리.
- [ ] ARTIFACT_RESULT의 비동기 수신·correlation 및 수신 파일 교차 확인.
- [ ] 형식·백신 검사, 실패 시 보류, 동일 바이트의 최종 반출.
- [ ] 기존 artifact_list/export backend와 tool 가용성 연결. Runtime이 필요한 반출 요청의 READY 검사 추가.
- [ ] 기존 REQUIRE_APPROVAL 정책의 approver 연결. 도구 설명·수락 상태·비동기 결과·중복 호출 동작 합의.

Host 코드의 예상 수정 지점은 `schema/envelope.schema.json`의 EVENT_ACK 예외와 error 검증,
`schema/payload/HELLO*.schema.json` capability, `SECURITY_EVENT/EVENT_ACK/ARTIFACT_REQUEST/ARTIFACT_RESULT`,
`scrp/validate.py`의 프로파일 선택, `sender.py`의 capability/Telemetry 라우터,
`bootstrap.py`의 artifact_upload, `upload_server.py`의 Artifact handler,
`tool_availability.py`·`broker.py`의 Artifact 구현과 READY 검사,
`mcp_server.py`의 approver·후속 cleanup 연결이다.
기존 schema를 모든 세션에 전역 덮어쓰기하지 않는다.

## 12. Runner 구현과 검증 경계

1. `ArtifactExportSchema`가 기존 GUI schema를 합성하며 새 요청/결과만 검증한다.
2. `HostArtifactChannels`가 검증된 HELLO_ACK 자격으로 Telemetry를 비동기 시작하고 CHANNEL_ACK 후 후보 생산자를 연결한다.
3. `CandidateTelemetry::Observer`가 enqueue된 event_id와 안정화 관찰값을 `ArtifactExportSession`에 등록·무효화한다.
4. `ArtifactExportSession`은 최대 4096개 활성 후보·4096개 사용한 upload_id, 전송 1개·완료 슬롯 1개를 보유한다.
5. worker는 현재 Detector 세대와 파일 관찰값을 비교하고 `VerifiedFile`로 재검증한 소스를 64 KiB 버퍼로 전송한다.
6. Receiver만 완료를 원 correlation/connection에 송신한다. TERMINATE 결과는 후보/업로드/Telemetry 종료와 결과 drain 후 전송한다.
7. 실제 Windows Sandbox의 Runner 반출·종료는 독립 peer로 검증했다. 실제 Host 수신·검사·MCP 전체 반출은 Host 구현 후 수행한다.

공유 스키마는 **형태 검증 자료**다. 인증·권한·실제 시각·경로/핸들·길이/기한·중복/상관관계는 코드에서 검증해야 한다.
스키마 참조는 로컬 파일만 사용하고 예시 `$id` URL로 네트워크 요청하지 않는다.
검증 스크립트는 저장소 기존 방침대로 Git 제외 `tests/`에 보관한다.

| 반드시 확인할 시나리오 | 기대 결과 |
| --- | --- |
| 실행 후 한글 경로 TXT 생성 → 후보 → 요청 → 수신 → 검사 | Host 결과 파일과 원본 내용 일치, MCP에서 최종 결과 확인 |
| 같은 이벤트 재전송 / 같은 ID의 다른 payload | 중복 등록 없음 / 저장 거부 |
| 바쁜 상태의 두 번째 요청 | ARTIFACT_BUSY, 두 번째 전송 없음 |
| 삭제·변경·다른 세션·범위 밖/링크 후보 | 소스 접근/전송 거부 |
| 잘못된 토큰·만료·길이 불일치·용량 초과 | 최종 파일 없음, grant 재사용 안 됨 |
| 전송 중 heartbeat·종료 | 관리 채널 계속 처리, 종료 시 I/O 취소·핸들 정리 |
| 수신 완료 직후 응답 유실·결과 유실·Control 재연결 | 자동 재전송/반출 없음, 새 요청 필요 |
| 검사 탐지·오류·미설치·시간 초과 | EXPORTED로 전환하지 않음 |
| Host 재시작 | 이전 권한 거부, 남은 임시 파일 자동 공개 없음 |
| 구형 profile와 새 profile 혼합 | 실패가 명확함, 스키마를 추측해 자동 허용하지 않음 |

Runner의 빌드·로컬 검증 결과는 [Runner 구현 기록](artifact-export-implementation.md)에 기록한다.
실제 Sandbox의 Runner 반출·종료는 [독립 peer로 검증](artifact-sandbox-validation.md)했다. 실제 Host 수신·검사·MCP 전체 반출은 **미검증**이다.
Host 담당의 채택 revision·담당자·확인 날짜·실제 상호 운용 결과는 후속 작업에서 기록한다.

## 13. 2026-10-03 문서 검증 기록

- JSON Schema draft 2020-12 자체 검증 통과.
- 정상/실패 형태의 전체 Envelope 예제 14개 통과.
- 필수 필드 누락, 구형 필드, 임의 URL, 경로 순회, 잘못된 시각, 길이 상한, 잘못된 ACK/결과 조합 등
  거부 사례 38개 확인.
- 예제의 후보 ID·correlation_id·connection_id·HTTP body 길이 및 토큰 연결 확인.
- 기존 Host fed305b 스키마와의 의도된 비호환 지점 5개 확인: 후보 payload, ACK payload,
  ACK Envelope status/error, 요청 payload, 결과 payload.
- 관련 문서의 로컬 링크 22개 확인. 제품 소스 및 기존 재연결 작업 파일은 수정하지 않음.
- 로컬 재현: 저장소 루트에서 `python tests/artifact_export_contract_test.py`.
  Python jsonschema와 기존 `build/host-control-integration` checkout이 필요하다.
  해당 스크립트는 기존 정책대로 Git 제외이며 제품 빌드/배포 의존성이 아니다.
- C++ 빌드·실제 전송·검사·MCP·Sandbox E2E는 이번 문서 변경의 검증 대상이 아니며 실행하지 않았다.

## 14. 최신 Host 0690845 비교 검증

2026-10-03 사용자 지정 `feat/24-sandbox-launch`를 별도 읽기용 checkout으로 확보하고 commit을 고정했다.
Host 소스·스키마를 수정하지 않고 해당 checkout의 원본 `scrp.validate.parse_and_validate`를 호출했다.

| 문서 예제 | 최신 Host 형태 검증 결과 |
| --- | --- |
| CHANNEL_HELLO / CHANNEL_ACK | 통과. 실제 Telemetry 라우터가 있다는 뜻은 아님 |
| SECURITY_EVENT | INVALID_ARGUMENT: relative_path 추가 필드 거부, 기존 evidence 형식과 다름 |
| EVENT_ACK | PROTOCOL_DENIED: STORED가 기존 OK/ERROR enum에 없음 |
| ARTIFACT_REQUEST | INVALID_ARGUMENT: candidate_event_id/upload_token 추가 필드 거부 |
| ARTIFACT_RESULT | INVALID_ARGUMENT: candidate_event_id 추가 필드 거부, 기존 candidate_id/sha256 계약과 다름 |

- 기존 fed305b와 최신 Host의 공유 JSON Schema 21개 비교: 20개 완전 동일, OBSERVE는 설명의 업로드 포트만 변경.
  검증 제약은 21개 모두 같으므로 Artifact wire 불일치가 최신 브랜치에도 남아 있다.
- 기존 HELLO payload는 통과하고 artifact.export.v1 capability를 추가하면 현재 Host 스키마가 거부하는 것을 확인.
- 원본 ToolCatalog에서 artifact_list/export가 스키마에는 있으나 가용 도구에서 제외됨을 확인.
- 원본 Policy의 artifact_export 결과는 REQUIRE_APPROVAL임을 확인. approver hook의 실제 UI 연결은 실행하지 않음.
- HTTP 예제 Host를 17444로 정정. messages.schema.json의 제안 wire 형태는 변경하지 않음.
- 로컬 재현: `python tests/artifact_export_host_compat_test.py`.
  Python jsonschema, 최신 checkout `build/host-export-reference-0690845`, 기존 fed305b checkout이 필요하다.
- 실제 네트워크·서버 기동·Sandbox·MCP E2E 시험은 하지 않았다. Host 구현 존재 확인, 원본 validator 실행,
  기존 담당자의 실기 PASS 보고는 각각 별도의 증거다.
