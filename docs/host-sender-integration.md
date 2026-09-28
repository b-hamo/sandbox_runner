# Host Sender 연동 검증 (#13)

## 기준과 변경 범위

- Host: [b-hamo/host_control, feat/1-host-sender](https://github.com/b-hamo/host_control/tree/feat/1-host-sender)
- 검증 commit: `fed305b06dfcfdced406c316514cd3fd2e03a911`
- Runner: `feat/13-control-receiver`
- Host checkout의 소스 수정 없이 원본 `host/sender.py`와 `Connection` 클래스를 사용했다.

Host의 `schema/README.md`는 JSON Schema를 PDF보다 우선하는 기준으로 정의한다.
동시에 handshake·인자 한계·Artifact 일부 항목은 확정 대기라고 명시한다.
따라서 제품에는 명시적 `host-sender-fed305b` 호환 profile로 추가했으며,
최종 SCRP 동결이나 기존 artifact-candidate-v1 계약 변경으로 취급하지 않는다.

`HostSenderSchema`는 해당 commit의 필요한 필드·enum·닫힌 객체 규칙을 C++로 검증한다.
런타임에 JSON Schema 파일을 읽는 범용 validator는 아니다. 문자열 상한은 보수적으로 UTF-8 바이트를
사용하고 keyboard.type은 4 KiB를 추가로 제한한다. Host 스키마가 변경되면 별도 호환 검증이 필요하다.
Artifact 요청의 candidate_id는 Host 초안 형태만 검사하여 미지원 오류를 반환하며 #11 후보 ID로 해석하지 않는다.

## 발견한 차이와 Runner 수정

| 차이 | 처리 |
| --- | --- |
| Host는 평문 WS만 수신하고 token을 검증하지 않음 | 제품 WSS/TLS 검증 유지. 테스트용 로컬 TLS 중계로 원본 Sender에 연결 |
| Host timestamp는 소수점 6자리 | 공통 Envelope 파서를 Z 접미사·소수점 1~6자리로 확장, 잘못된 날짜·형식은 거부 |
| Receiver에 제품 handshake adapter와 실행 경로가 없음 | HostSenderSchema, `load_control_context`, `control_session`, `--control-context` 추가 |
| 수신만 있고 응답 송신이 없음 | ReplyHandlers, 응답 schema·한도 검증, 방향별 sequence·correlation/task/action 연결 |
| Host OBSERVE도 task/action ID가 필수 | OBSERVE 검증·중복 Action ID 차단에 포함 |
| Host 데모는 GUI 구현을 가정함 | 제품은 빈 GUI Capability, DEGRADED/worker_alive=false, 미지원 ERROR를 반환 |

네트워크 경로는 `Runner WinHTTP WSS → loopback TLS 중계 → 원본 Host WS`다.
중계는 테스트 스크립트의 임시 구성이고 제품 배포물에 포함하지 않는다. 인증서 검증 해제,
WS 자동 fallback, Host 소스 패치는 없다. Host 자체 TLS·token 검증이 완성된 것은 아니다.

## 제품 실행

```powershell
cmake -S . -B build/issue13/product -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/issue13/product
./build/issue13/product/sandbox_runner.exe --control-context C:/RunnerBootstrap/control.json
```

설정 예제(실제 Host/WSS endpoint, 세션, 자격, 만료 시각으로 교체):

```json
{
  "control_contract": "host-sender-fed305b",
  "session_id": "SES-example",
  "runtime_id": "RT-example",
  "generation": 1,
  "endpoint": "wss://host.example:17443/scrp/v1/control",
  "credential": {
    "header_name": "Authorization",
    "header_value": "Bearer REPLACE_WITH_HOST_BOOTSTRAP_TOKEN",
    "expires_unix_seconds": 1798761600
  },
  "tls": {"trust": "windows-system", "leaf_sha256": ""}
}
```

Windows 인증서 저장소의 체인·호스트명 검증이 필수다. leaf_sha256은 선택적 추가 pin이다.
bootstrap 파일은 자격을 포함하므로 외부 launcher가 접근 권한·수명·제거를 관리한다.
파일에서 받은 ID와 세션을 그대로 사용하고 자체 발급하지 않는다.
수신 token 검증 책임은 Host에 있다. 현재 Sender의 WS endpoint를 위 설정에 그대로 넣으면 실패한다.

Control 모드는 Artifact 감시·Telemetry·GUI Worker를 시작하지 않는다.
HEARTBEAT → ALIVE, STATE_REQUEST → STATE_RESULT, TERMINATE → TERMINATE_RESULT를 처리한다.
조회한 실행 기록은 UNKNOWN이다. TERMINATE_RESULT 송신 후 worker를 join하고 정상 종료한다.
OBSERVE/ACTION_REQUEST/ARTIFACT_REQUEST에는 UNSUPPORTED_TYPE를 반환한다.
Host가 데모를 중단하고 연결을 닫으면 Runner는 연결 실패로 종료한다.

## 2026-09-28 검증 결과

| 검증 | 결과·의미 |
| --- | --- |
| 제품 EXE ↔ 실제 Host Connection | HELLO, ALIVE, STATE_RESULT, 미지원 ERROR, TERMINATE_RESULT 왕복 통과 |
| 잘못된 HEARTBEAT payload | 핸들러 전달·ALIVE 없이 실패 종료 |
| 원본 sender.py 데모 ↔ 제품 EXE | HELLO 성공, OBSERVE에서 UNSUPPORTED_TYPE 수신 후 Host가 예상대로 중단. GUI 데모 성공이 아님 |
| 원본 sender.py 데모 ↔ 테스트 전용 C++ 핸들러 | OBSERVE → 클릭 → 한글 입력 → Heartbeat → 상태 → 종료 전체 메시지 왕복, Host `demo complete` 확인 |
| 테스트 GUI 응답 | OBSERVE_RESULT는 합성 메타데이터. 입력은 ACK=REJECTED, ACTION_RESULT=BLOCKED, input_delivered=false. 실제 캡처·업로드·입력 없음 |
| Host 스키마 검증 | 실제 응답의 sequence/correlation/task/action 및 payload를 원본 Python 검증기가 수용 |
| UCRT64 Release / 회귀 | GCC 16.2.0 제품 빌드, CTest 9/9(기존 7개 + Host 스키마·응답 경계 6케이스) 통과 |
| 실제 Control WSS | 기존 TLS/분할 프레임/잘못된 세션·payload/크기/취소 10개 재검증 통과 |

실행에 쓴 Python 환경은 3.14, websockets 17.1, jsonschema 4.26.0, cryptography 50.0.1이다.
버전은 검증 기록이며 팀 동결값이 아니다. 모든 테스트 자식 프로세스 및 임시 CurrentUser Root 인증서를 정리했다.
Host 원본 소스의 변경 없음은 `git -C build/host-control-integration status --short`로 확인했다.
실제 Sandbox↔Host 네트워크, 실제 GUI Worker, 업로드, Host 인증 강제, Control↔Telemetry 동시 기동은 미검증/후속이다.

## 로컬 재현

기존 방침에 따라 `tests/`는 Git 제외 로컬 검증 코드다. 새 checkout에는 포함되지 않는다.

```powershell
git clone --single-branch --branch feat/1-host-sender https://github.com/b-hamo/host_control.git build/host-control-integration
git -C build/host-control-integration rev-parse HEAD
python -m venv build/host-integration-venv
./build/host-integration-venv/Scripts/python.exe -m pip install -r build/host-control-integration/requirements.txt cryptography
cmake -S tests -B build/issue13/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OBJECT_PATH_MAX=190
cmake --build build/issue13/tests
ctest --test-dir build/issue13/tests --output-on-failure
./build/host-integration-venv/Scripts/python.exe -X utf8 tests/host_sender_integration.py
```

원본 sender.py는 고정 포트 17443 및 0.0.0.0 바인딩을 사용하므로 테스트 전 해당 포트가 비어 있어야 한다.
테스트는 기존 프로세스를 종료하거나 방화벽 규칙을 변경하지 않는다.
TLS 중계/Host Connection 테스트 listener는 loopback 임시 포트를 사용한다.
스크립트는 새 테스트 인증서 한 개를 현재 사용자 Root 저장소에 일시 등록하고 finally에서 정확히 제거한다.
