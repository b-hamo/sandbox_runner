# ARTIFACT_CANDIDATE 전송 계약 — artifact-candidate-v1

Issue #23의 [artifact-export-v1](artifact-export-contract.md)은 이 후보 payload/ACK를 그대로 재사용하고
후보 조회·승인된 요청·Runner HTTPS 업로드를 제품에 연결한다. 아래 제외 범위는 기존 #11의 범위이며
현재 제품 전체의 미구현 목록이 아니다. Host 채택과 최종 반출은 별도 구현·검증 대상이다.

## 상태와 범위

Issue #11에서 정의한 **Runner 구현·검증용 계약안**이다. Host 합의가 완료된 기존 SCRP 규약으로
간주하지 않는다. 2026-09-27 기준 Host 검토자·승인 기록·실제 Host endpoint는 제공되지 않았다.
Host 공유용 문서와 예제를 이 파일에 마련했으며, **Host 전달·합의 및 실제 Host 호환성 검증은 대기**다.
사용자 확인: 공유 위치 미정이므로 이번 작업은 문서와 대기 상태 기록까지 수행한다.
공유 후 검토자, 날짜, 합의 revision과 호환성 결과를 이 절에 기록한다.

명시적으로 이 계약을 선택한 실행에서만 활성화한다. 선택명은 로컬 Context의
`event_contract: "artifact-candidate-v1"`이며 Envelope의 기존 `version: "1.0"`을 바꾸지 않는다.
Host와 호환되지 않는 계약 변경은 기존 선택명의 의미를 몰래 바꾸지 않고 새 revision으로 합의한다.

Artifact Broker 등록, ARTIFACT_REQUEST, Runner 업로드, Host 검사·반출, 무효화 wire 전송은 제외한다.
CHANNEL_HELLO/ACK와 인증 헤더는 기존 외부 handshake 계약을 사용한다.

## SECURITY_EVENT payload

다음 네 필드만 허용하는 닫힌 JSON object다. 누락·null·추가 필드·잘못된 타입을 거부한다.

| 필드 | 타입·의미 |
| --- | --- |
| `event_id` | 소문자 UUIDv4. 확정된 후보 관찰 이벤트의 논리적 ID |
| `observed_at` | 유효한 UTC RFC3339 시각. `YYYY-MM-DDTHH:mm:ssZ` 또는 밀리초 3자리 포함 형식. Runner는 밀리초 형식 생성 |
| `category` | 문자열 `ARTIFACT_CANDIDATE` |
| `relative_path` | Output 기준 UTF-8 Windows 상대 경로 |

```json
{
  "event_id": "b64fbf52-4f4e-4c99-994c-e7f0e8d29101",
  "observed_at": "2026-09-27T13:00:00.123Z",
  "category": "ARTIFACT_CANDIDATE",
  "relative_path": "reports\\결과.txt"
}
```

경로 구분자는 역슬래시(U+005C)이며 JSON 문자열에서는 `\\`로 escape한다. 대소문자·Unicode
표기를 보존하고 slash 변환·URL decode·Unicode 정규화는 하지 않는다. 빈 경로·빈 component,
`.`·`..`, 절대/UNC/device 경로, slash, colon/ADS, 제어문자, Windows 금지 문자,
component 끝의 점·공백, 마지막 구분자를 거부한다. 세션 Output의 절대 경로는 전송하지 않는다.
전송 크기는 기존 TelemetryClient의 Envelope 포함 16 KiB 제한을 따른다.

경로는 비신뢰 관찰 메타데이터이며 Host 저장 경로나 파일 접근 권한이 아니다.
이 schema 검증은 파일 재접근 시의 경로·reparse point 검증을 대체하지 않는다.
session_id·runtime_id·Runtime generation은 기존 Envelope에만 있으며 payload에 중복하지 않는다.
로컬 파일 변경 세대, candidate_id, 해시·MIME·크기·안전 판정은 payload에 넣지 않는다.

## 식별자·재전송·새 관찰

- `event_id`: 후보 확정 시 생성한다. 동일 이벤트 재전송은 ID·시각·경로를 포함한 payload 전체를 유지한다.
- `message_id`: 송신 시도별 Envelope ID. 재전송 시 새 message_id·nonce·timestamp와 연결별 sequence를 사용한다.
- 재수정 후 다시 안정화되면 같은 상대 경로라도 새 `event_id`를 부여한다. 시각은 실제 관찰 시각이며
  시계 해상도·조정 때문에 시각만으로 이벤트를 식별하거나 순서를 결정하지 않는다.
- Host는 세션/Runtime 문맥 안에서 event_id로 중복 저장을 막고 이미 저장한 동일 이벤트에도 STORED를 응답한다.
  같은 ID에 다른 payload가 오면 성공 처리하지 않는다.
- 별도 candidate_id를 도입하지 않는다. event_id는 파일의 영구 ID나 Broker 등록 ID가 아니다.
- 새 이벤트가 이전 후보를 자동으로 대체하거나 무효화한다는 의미는 없다. Host 후보 유효성 정책은 후속 계약이다.

## EVENT_ACK

공통 Envelope 검증(세션·Runtime generation·연결·sequence·시각·중복 ID/nonce)을 통과하고,
`type`이 EVENT_ACK, `correlation_id`가 **현재 송신 시도의 message_id**,
payload.event_id가 **현재 in-flight event_id**와 일치해야 한다. task_id·action_id는 null이다.
이벤트 payload의 observed_at은 재전송해도 갱신하지 않으므로 수신 Envelope 시각의 60초 제한을 적용하지 않는다.

아래는 기존 공통 Envelope 중 ACK 관련 필드만 발췌한 예제다. 실제 wire에는 모든 Envelope 필드가 필요하다.

저장 성공:

```json
{
  "type": "EVENT_ACK",
  "correlation_id": "1ebaa616-906c-43c1-a682-1c7692f782d8",
  "status": "STORED",
  "error": null,
  "payload": {"event_id": "b64fbf52-4f4e-4c99-994c-e7f0e8d29101"}
}
```

Host는 이벤트 저장 완료 후 응답한다. 이는 소켓 수신만의 확인이 아니며,
**Artifact Broker 후보 등록 완료·승인·업로드 허가·안전 판정도 아니다.**
Host의 이벤트 저장 이후 Broker 처리 실패에 대한 내부 재처리는 Runner ACK 책임 밖이다.

명시적 저장 거부:

```json
{
  "type": "EVENT_ACK",
  "correlation_id": "1ebaa616-906c-43c1-a682-1c7692f782d8",
  "status": "REJECTED",
  "error": {"code": "STORAGE_REJECTED"},
  "payload": {"event_id": "b64fbf52-4f4e-4c99-994c-e7f0e8d29101"}
}
```

ACK payload는 event_id 하나만 허용한다. STORED에는 error가 null이어야 하며,
REJECTED에는 위 code 하나만 허용한다. 지원하지 않는 status/code, 추가 필드, null/형식 오류,
다른 event_id·상관 ID는 잘못된 ACK다. peer의 임의 오류 문자열을 로그에 복사하지 않는다.

| 결과 | Pending | 재시도·진단 |
| --- | --- | --- |
| 유효한 STORED | 해당 이벤트 제거 | 해당 이벤트 재전송 종료 |
| 유효한 REJECTED | 유지 | 저장 거부 고정 진단, 연결 정리 후 기존 제한된 재연결·재전송 |
| 잘못된 ACK 또는 ACK 시간 초과 | 유지 | 일반 연결/ACK 실패 진단, 동일한 기존 재연결·재전송 |
| 재연결 횟수 소진 | 유지 | DISCONNECTED·오류 노출. 자동 재시도 중단, 보관 만료 확인은 지속 |
| 보관 기간 만료 | 제거 | expired/error 누계 증가. ACK 성공으로 간주하지 않음 |
| Runner 종료 | Client 생존 중 진단 가능 | 무제한 drain 없음, Client 소멸 시 메모리 해제 |

기본 재연결은 1·2·4·8초 + jitter, 보관은 enqueue 기준 5분이다. 거부·재전송이 보관 시계를 초기화하지 않는다.
이 revision은 영구/일시 거부를 구분하지 않으며 거부를 성공·즉시 폐기로 처리하지 않는다.
따라서 지속 거부도 제한 횟수만 재시도하고 첫 in-flight 이벤트 뒤의 처리가 지연될 수 있다.
영구 거부 즉시 폐기·격리 큐·재시도 재개 기능은 구현하지 않았다. 기존 정책을 바꿀 때 별도 합의한다.

## 제품 Context 연결

기존 Context JSON에 다음 선택 필드 한 개를 추가한다.

```json
{"event_contract": "artifact-candidate-v1"}
```

이 발췌만으로는 실행할 수 없으며 기존 세션·endpoint·credential·TLS·handshake 필드도 모두 필요하다.
`load_session_context()`가 기존 handshake schema를 `ArtifactCandidateAdapter`로 감싼다.
같은 adapter가 TelemetrySchema와 CandidateEventContract를 구현하므로 `wmain()`의 기존
dynamic_pointer_cast와 CandidateTelemetry 연결이 실제로 활성화된다.
필드가 없으면 기존 handshake-only 동작을 유지하며 잘못된 값·null은 시작 전에 거부한다.
명시적 선택은 Host 합의 완료의 증명이 아니므로 합의 상태는 위 기록을 따로 확인한다.

handshake ACK payload를 `{}`로 합의하면 기존 주입 adapter도 Envelope의 동적 connection_id를
읽을 수 있다. connection_id를 handshake.ack_payload에 고정하면 재연결 시 그 고정값만 수용한다.
이 이슈에서 CHANNEL_ACK schema를 새로 정의하거나 동적으로 협상하지 않는다.

## 검증 구분

자동화 검증은 production adapter·계약 validator, 기존 fake socket seam, 실제 제품 EXE와
임시 trust를 사용하는 loopback WSS peer로 수행한다. 실제 Host 구현과의 상호 운용성이나
Windows Sandbox 배포 검증을 대체하지 않는다. 실행 결과는 telemetry.md에 기록한다.
