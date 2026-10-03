# Runner Artifact 반출 구현 기록 — Issue #23

## 적용과 실행

`develop` 452e1b5의 실제 GUI 제품 기반에 `artifact-export-v1`을 구현했다. 기존 작업 폴더의 `feat/13-control-receiver`와 미커밋 소스/문서는 보존하고 별도 `feat/23-artifact-export`에서 작업했다. 예전 계약 문서의 관리 전용/no-GUI/제품 미구현 설명을 현재 제품에 맞춰 정리했다. Host/MCP 코드는 변경하지 않았다.

기존 `--host-bootstrap`을 사용한다. 필수 bootstrap 11필드와 선택 `host_certificate_sha256`은 유지하며 다음 두 필드를 함께 추가한다.

```json
{
  "control_contract": "artifact-export-v1",
  "artifact_upload": {"port": 17444, "path": "/scrp/v1/artifacts/"}
}
```

17444는 예시다. Host 실제 Artifact HTTPS 수신 포트를 기록한다. 주소는 기존 bootstrap/명시 Host IPv4로 고정하며 요청에서 URL·파일 경로를 받지 않는다. 두 필드 중 하나만 있거나 profile/포트/경로/미정의 필드가 잘못되면 접속 전에 실패한다. Observation 업로드 설정·자격을 Artifact에 재사용하지 않는다. `--control-context`는 기존 관리 모드다.

## 내부 인터페이스와 동작

- `ArtifactExportSchema(delegate)`는 기존 GUI schema를 합성한다. HELLO에 `artifact.export.v1`을 광고하고 검증된 HELLO_ACK의 허용 capability·같은 세션/connection·Telemetry 자격을 고정한다. 기존 GUI validator에는 Artifact capability만 제거한 ACK 사본을 전달한다.
- `HostArtifactChannels`가 별도 Telemetry WSS를 비동기 시작한다. 기존 GUI coverage용 알림 감시는 HELLO 전에 준비하고, CHANNEL_ACK 후 같은 watcher에 후보 sink를 연결한다. 후보 준비 전 변경과 기존 파일의 초기 스캔은 없다. Host는 채널 준비와 기존 GUI READY 검증 후 작업을 시작한다.
- `CandidateStatus`는 안정화 당시 `FileObservation`과 `has_observation`을 전달한다. pending·삭제·감시 오류·종료는 유효한 관찰값을 제공하지 않는다.
- `CandidateTelemetry::Observer(observation,status,current)`가 wire 송신 가능 전 후보를 임시 등록하고 enqueue 실패 시 rollback한다. Host가 SECURITY_EVENT를 받은 즉시 요청하더라도 registry 등록이 늦는 경쟁을 막는다. 기존 observer 없는 독립 모드 동작은 유지한다.
- `ArtifactExportSession`은 세션/Control connection에 고정된 event_id→경로/로컬 generation/관찰값 registry다. 최대 후보 4096개·사용 upload_id 4096개, 전송 1개·완료 슬롯 1개다. 바쁜 요청은 ARTIFACT_BUSY로 완료하며 별도 대기열/자동 재시도는 없다.
- worker는 Detector의 현재 generation과 관찰값을 확인하고 `open_verified_file()`로 소스를 재검증한다. `VerifiedFile`은 move-only RAII이며 파일과 드라이브부터 전체 디렉터리 체인의 핸들을 유지한다. 쓰기·삭제 공유를 제한하고 reparse/ADS/하드링크·비일반 파일·크기 초과를 거부한다. 파일을 경로로 다시 열지 않는다.
- `artifact_transfer::HttpsUploader`는 별도 Bearer token, 정확한 Content-Length와 application/octet-stream으로 64 KiB 버퍼를 스트리밍한다. 0바이트 파일도 허용한다. Windows chain/hostname + leaf pin을 검증하고 pin은 SENDING_REQUEST에서 헤더 전송 전에 확인한다. proxy·redirect·자동 인증·평문 전환·이어받기·재전송은 없다.
- 요청 수락 시 기록한 monotonic 기한과 UTC 기한을 모두 적용한다. 후속 시계 역행이나 worker 대기가 권한 수명을 연장하지 않는다. HTTP 201 + 빈 응답만 UPLOADED이며 다른 status/응답 유실/비어 있지 않은 body는 실패다.
- worker는 WSS에 쓰지 않는다. `poll()`의 완료를 Receiver가 원 요청 correlation·connection·송신 sequence로 보낸다. Artifact 초기 ACK는 없다. 파일 내용·해시·최종 파일 참조는 ARTIFACT_RESULT에 넣지 않는다.
- 채널 장애·유실·감시 오류·lease 만료는 후보와 활성 업로드를 무효화한다. 같은 실행의 반출을 자동 재활성화하지 않는다. TERMINATE는 신규 요청 차단 → sink 분리 → 후보/업로드/Telemetry join → 결과 drain → GUI 종료 결과 순서다.
- 정상 최종 Control 결과 뒤 최대 500ms의 WebSocket send-channel shutdown을 거친다. 즉시 handle close로 마지막 TERMINATE_RESULT가 유실되던 실제 EXE 타이밍을 보완했다. 장애·외부 취소는 기존 즉시 I/O 취소를 유지한다. 네트워크 송신 완료는 Host 업무 처리 ACK를 보장하지 않는다.

권한·형태·오류 코드는 [반출 계약](artifact-export-contract.md)과 [JSON Schema](contracts/artifact-export-v1/messages.schema.json)를 따른다. 새 Schema는 문서/검증 자료이며 제품 설정 파일이 아니다. 일반 파일 전송 자체에 확장자·UTF-8·악성코드 검사를 넣지 않았으며 최종 TXT 허용 정책과 검사는 Host 책임이다.

## Host 담당 연결 순서

1. bootstrap writer에 위 필드와 실제 HTTPS 포트를 추가한다. HELLO/ACK capability enum과 sender 허용 목록에 artifact.export.v1을 넣고 세션 profile을 고정한다.
2. HELLO_ACK의 Telemetry token을 실제 세션 registry에 등록한다. Control과 같은 포트의 `/scrp/v1/telemetry` WSS에 별도 Bearer 인증·세션 binding, CHANNEL_HELLO/ACK, 네 필드 후보와 STORED/REJECTED EVENT_ACK를 구현한다.
3. 이벤트 저장·중복 방지 후 Broker의 artifact_id를 발급한다. artifact_list는 후보를 PENDING으로 제공하고 수신 전 크기·해시·MIME은 미확인으로 둔다. 무효화 wire가 없으므로 목록은 과거 보고를 포함하며 Runner 요청 거부를 처리해야 한다.
4. artifact_export(artifact_id,reason)를 기존 READY·REQUIRE_APPROVAL 정책에 연결한다. 새 upload_id·43자 token·deadline·max_bytes를 grant registry에 먼저 등록하고 해당 후보 event_id로 ARTIFACT_REQUEST를 보낸다. MCP는 수락 상태를 반환하고 진행은 artifact_list로 확인한다.
5. HTTPS `/scrp/v1/artifacts/<upload_id>`에 전용 수신기를 추가한다. 별도 인증·scope·일회성 claim·기한·Content-Length/실제 크기·저장소 상한을 검증하며 비공개 임시 저장소로 스트리밍한다. 수신·닫기가 완료된 뒤 크기/SHA-256을 고정하고 빈 201을 반환한다. 실패 부분 파일과 grant를 재사용하지 않는다.
6. 원 ARTIFACT_RESULT의 correlation/connection/후보/upload_id/bytes_sent가 실제 HTTP 수신 완료 기록과 일치할 때만 검사한다. 기존 PNG 해시 응답 경로와 혼용하지 않는다.
7. 허용 형식·백신·정책을 통과한 동일 불변 바이트만 최종 공개하고 EXPORTED 및 실제 결과 참조를 제공한다. Scanner 오류/미설치/치료·수정/시간 초과는 통과가 아니다. 종료·재시작·Runtime generation 경계에서 미반출 grant/작업을 취소한다.

상세 Host 상태·idempotency·승인/검사 의미는 [계약 10~11절](artifact-export-contract.md#10-host-검사최종-반출과-종료)에 있다. Runner 201 수신은 SAFE나 EXPORTED가 아니다.

## 2026-10-03 검증

Windows x64 MSYS2 UCRT64 GCC 16.2.0, CMake/Ninja Release 제품 빌드 및 일반 PowerShell `--help` 실행 통과. JsonCpp 정적 연결과 기존 C++17 GUI 기반을 유지한다. EXE 약 4.2 MiB이며 objdump에서 Windows 기본 DLL만 확인했다. 도구/라이브러리 버전은 이번 검증 환경이며 팀 동결값이 아니다.

| 검증 | 결과 |
| --- | --- |
| 기존 로컬 CTest | 최종 12/12 통과: Control/schema/reply, Artifact/candidate/Telemetry/lifecycle, GUI Runtime/session, observation, watcher |
| Receiver 포화 native test | GUI 33건·Artifact 1건의 두 등록 순서, 추가 Artifact 즉시 busy, heartbeat·최종 35개 결과 drain, 34번째 GUI의 handler 실행 전 거부 통과 |
| 기존 bootstrap SHA-256 입력 회귀 | 63/63 통과. 이전/신규 필드·누락/추가·타입·해시/PEM 불일치. 접속 없는 admission 시험 |
| 신규 Artifact bootstrap 입력 | 28/28 거부 경계 통과: profile/settings pair·미정의 필드·타입·경로·포트. 자격 로그 없음 |
| 프로토콜·export worker native tests | 엄격 schema/협상·binding·결과 조합, 비동기 성공·busy·무효화·종료·건강 상태·identity/generation·크기/기한·ID 재사용/상한, provisional observer/rollback 통과 |
| VerifiedFile native tests | 한글·빈 파일·핸들 정리·writer/delete/부모 rename 차단·stale·순회·ADS·하드링크·실제 junction 거부 통과 |
| 실제 WinHTTP HTTPS | 16개 시나리오 통과: 다중 버퍼/0바이트, TLS trust/hostname/pin, 거부 시 헤더/바이트 0, status/응답 body/유실, 재사용·만료·monotonic budget·취소 |
| 실제 WinHTTP WSS | 정상 텍스트 왕복·미신뢰·hostname/pin 불일치 4개 통과. 인증서 거부 시 HTTP 헤더 0바이트 |
| 공유 문서·Schema | draft 자체 검증, 전체 Envelope 예제 14개, 요청 거부 16개 및 로컬 문서 링크 통과 |
| 실제 제품 EXE + 독립 TLS peer | 한글/빈 파일 후보→ACK→요청→정확한 PUT→결과, stale/reuse/size/expiry, HTTP403/nonempty201, busy, stalled PUT 중 heartbeat<1초, 종료 취소·join·TERMINATE_RESULT·exit0. 종료 보완 후 3회 연속 및 최종 TLS pin 변경 빌드 1회 통과 |

기존 lifecycle의 엄격한 프로세스 핸들 수 assert가 간헐적으로 +1을 기록했다. 같은 시험은 변경 전 baseline에서도 재현됐다(current 20회 중1, baseline 20회 중2). 250cycle 비교에서 +1 이후 추가 증가 없이 유지됐으며 원인은 확정하지 않았다. 이번 변경의 지속 누수 증거로 해석하지 않으며 최종 CTest 통과와 이 기존 불안정 측정을 구분한다.

로컬 테스트는 기존 방침대로 `tests/`에만 보관한다. 제품 CMake는 테스트를 참조하지 않는다. 임시 인증서는 매 시험 정확한 thumbprint로 삭제하고 테스트 UUID 세션만 정리했다. 프로세스/포트·자격/fixture·EXE는 커밋하지 않는다.

```powershell
# 같은 UCRT64 도구 환경에서
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\sandbox_runner.exe --help
# 로컬 테스트가 있는 작업 공간에서만
cmake -S tests -B build/local-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/local-tests
ctest --test-dir build/local-tests --output-on-failure
python tests/artifact_product_test.py build/sandbox_runner.exe
```

Python peer에는 cryptography가 필요하다. 실제 시험은 기존 Host용 venv 또는 bundled Python을 사용했으며 제품 배포 의존성이 아니다.

## 미검증·남은 결정

실제 Host Artifact Broker/수신기/백신/MCP와 Windows Sandbox 전체 반출은 아직 미검증이다. 위 네트워크 시험은 실제 Windows 제품 EXE와 독립 loopback peer이며 Host 정책·Scanner 성공을 대신하지 않는다. symlink 생성 자체는 Windows 권한 1314로 실행하지 못했으며 실제 junction/reparse 거부를 별도로 검증했다.

Host 채택 revision·READY/Telemetry 작업 시작 순서·승인 UX·수신/검사 정책·실제 파일 참조를 연결하고 전체 발표 시나리오를 검증해야 한다. 신규 상주 프로세스나 Sandbox 빌드 도구·별도 검사 모듈은 Runner에 추가하지 않았다.
