# GUI 제품 연결 기반과 최소 Host 계약안 (#17)

## 상태와 확인 기준

이 문서는 **합의 전 제안**이다. 새 wire 필드·메시지·토큰 형식을 확정하지 않는다.
제품 lifecycle과 HTTPS 전송 기반은 구현했지만 **일반 EXE의 GUI 활성화는 미완료**다.
기존 테스트 승인 정보와 HTTPS 수신기는 운영 계약이 아니다.

- Runner 원격 develop: `43bda835c81abdea827b917cd1b63f37591082b0`.
- Host 원격 develop/main: `966b308ae39d6933135403ed7d890e4a192197e5` (초기 커밋).
- Host 검토 checkout: `feat/13-observe-wait`, `d939215b849286375e1c87aff9673ee92d57bd1b`.
  startup/heartbeat/broker/MCP 구현을 포함하는 최신 기능 계열을 읽기 전용으로 검토했다.
- 이전 Sandbox 실험 기준: `feat/3-auth-tls`, `973822e55196074a46fecd207f5acd16d5cde5ab`.
- 중단 커밋 재개 시 사용자 제공 및 로컬 AGENTS.md를 확인하고 적용했다.

## 코드에서 확인한 충족 조건과 공백

| 조건 | 이미 있는 근거 | 아직 필요한 것 |
| --- | --- | --- |
| 세션 인증 | Host `session_registry.py`, `sender.handle`: 일회성 bootstrap, HELLO의 session/runtime/generation 대조. Runner Receiver의 Envelope·sequence·replay 검증 | 별도 GUI 로그인/동일 세션 토큰을 추가할 이유 없음 |
| capability | HELLO/HELLO_ACK `allowed_capabilities`; GuiSession이 grant와 교집합만 사용 | 준비 단계에서 observe만 허용할지, input의 효력 발생 시점을 정의 |
| lease | Host `RuntimeSession._heartbeat`: `lease_expires_at`; Runner는 최대 15초 monotonic lease와 만료 후 재활성화 금지 | 첫 HEARTBEAT 전 초기 lease와 시작 검증 중의 갱신 시점 합의. 두 번째 lease 체계 불필요 |
| 정책 | ACTION_REQUEST에 `policy_version` 존재 | Runner가 처음 기대할 정책 revision은 전달되지 않음. 첫 Action에서 임의 채택하지 말고 bootstrap/인증된 협상에 바인딩 |
| 시작 확인 | `startup.verify_runtime`: STATE, OBSERVE의 captured_at, HEARTBEAT 확인 후 Host 내부 `verified=True` | 현재는 PNG 수신/해시/디코딩 확인이 없고, 결과가 Runner로 전달되지 않음 |
| 네트워크 정책 | Runner HostGrant의 `network_policy_verified` 요구 | 실제 방화벽 검증 주체·증거·수명·실패 시 차단 통지. Host startup 코드에 이 검사가 없음 |
| 모니터링 | Host StartupProfile 기본 `required_monitoring=("file",)` | Runner GUI HELLO는 coverage 모두 false. 실제 모니터 연결과 건강 상태 증거 필요. Output watcher만으로 전체 file coverage를 true로 주장하지 않음 |
| PNG 전송 | OBSERVE 스키마에 PUT `/scrp/v1/observations/<upload_id>` 명시 | `RuntimeSession._observe`는 UUID를 만들 뿐 등록부·업로드 credential·상한·만료를 발급하지 않음. `broker.py`도 이미지 경로 미구현으로 명시 |
| bootstrap/TLS | Host `bootstrap.py`는 host/port/token/PEM 형식을 생성 | Runner `--control-context` 형식과 다름. 신뢰된 launcher 변환 또는 직접 reader 합의. Host README의 호스트명 검사 해제 안내는 채택하지 않음. SAN과 신뢰 체인이 유효한 인증서 필요 |

HELLO_ACK 스키마는 닫힌 객체이고 startup 승인·policy·upload scope가 없다.
OBSERVE도 `display_id`, `capture_format`, `upload_id`만 받는다. 기존 메시지에 몰래 필드를 추가하지 않는다.
Host의 reconnect/telemetry credential은 이미지 업로드 권한이 아니며 재사용하지 않는다.

## 별도 승인 절차가 필요한가

**별도 인증 절차는 필요하지 않다. 다만 입력 허용 시점과 시작 검증 결과의 신뢰 근거는 필요하다.**
현재 `HostGrant`의 세 가지 bool은 내부 C++ 인터페이스이지 확정된 프로토콜 요구가 아니다.
이 bool을 만족시키려고 새 승인 서버나 별도 토큰을 반드시 만드는 것은 과하다.
기존 인증된 Control 연결, 세션·세대, 협상 capability, 정책 revision, HEARTBEAT lease를 재사용할 수 있다.

결정할 수 있는 두 방식은 다음과 같다.

1. **권장: Control의 준비/입력허용 단계를 명시한다.** 인증 후 제한된 STATE/OBSERVE/HEARTBEAT/
   TERMINATE만 허용하고 입력은 막는다. Host가 PNG 실수신·검증과 필수 정책/모니터 확인 후 같은
   인증된 채널로 입력 허용 상태를 알린다. 기존 상태/lease 메시지의 버전별 확장인지 새 메시지인지는
   Host와 결정한다. 추가 로그인이나 두 번째 lease는 만들지 않는다.
2. **암묵적 승인도 합의 가능:** Host가 모든 시작 검증을 통과한 후에만 ACTION_REQUEST를 발행한다는
   계약을 보안 경계로 삼는다. Runner는 첫 인증된 Action을 입력 허용의 근거로 사용한다.
   이 경우 Runtime의 현재 사전 HostGrant 요구를 명시적으로 재설계해야 하며, Host 네트워크·모니터
   검증 누락을 해결하고 정책 바인딩/취소/초기 lease를 먼저 정의해야 한다. 현재 코드가 이미 이 계약을
   보장한다고 가정하지 않는다.

현 구조에는 순환 의존성이 있다: GuiSession은 HELLO_ACK 직후 사전 승인을 요구하지만,
최신 Host는 그 이후 STATE와 첫 OBSERVE를 받아야 startup을 통과한다.
기존 내부 `prepare/startup_probe` 연결점만 연결해 이 문제를 해결했다고 주장할 수 없다.
권장 방식이 채택되면 Runtime Prepared에서 캡처·업로드만 허용하는 경로와 InputEnabled 전이를
후속 구현하고, startup probe를 별도의 두 번째 캡처/승인 API로 중복 만들 필요가 없게 한다.

## PNG 업로드 최소 계약안 — 값과 표현은 결정 필요

아래는 필요한 **의미**이며 JSON 필드 이름을 제안 코드로 고정하지 않는다.

| 결정 항목 | 최소 요구 / 제안 |
| --- | --- |
| 발급 시점 | Host가 OBSERVE를 보내기 전에 수신 서비스에 upload_id와 scope를 원자적으로 등록 |
| 바인딩 | session/runtime/generation, connection, task/action, upload_id, observation 목적. 다른 요청/세대로 재사용 금지 |
| 대상 | 신뢰된 배포 설정의 HTTPS origin + 기존 명세의 고정 observations 경로. Guest 제공 URL·파일 경로 금지 |
| 인증 | 업로드 전용 일회성 credential, 또는 충분한 entropy의 upload_id 자체를 capability로 취급하는 명시적 합의. 후자는 URL 로그 노출과 상한/만료 관리 위험을 수용해야 함. **전용 credential 방식을 권장** |
| 전달 | OBSERVE의 버전별 확장, 또는 인증된 grant 조회 endpoint 중 선택. bootstrap에는 origin·신뢰·정책만, 미래 Action의 임의 grant를 미리 넣지 않음 |
| 시간·크기 | 발급 만료와 전체 전송 deadline, bytes/pixels 상한. Runner 현재 hard cap은 8 MiB/16 MP. 운영값·clock skew는 합의 |
| 완료 | HTTPS 수신기가 전체 바이트를 저장·PNG 디코딩하고 결과를 조회 가능하게 한 뒤 반환하는 정확한 성공 status를 합의. 202/단순 접수는 완료로 간주하지 않음 |
| 검증 | Host가 OBSERVE_RESULT의 upload_id, SHA-256, width/height를 **실제 수신 바이트**와 대조. startup은 디코딩 확인까지 수행 |
| 실패·재시도 | Runner는 grant를 I/O 전 소모하고 자동 재시도하지 않음. 성공 응답 유실 시 불확실한 결과를 성공으로 추정하지 않고, Host가 새 OBSERVE/새 grant 발급 |
| 취소·보존 | 종료/단절/세대 교체 시 미사용 grant 폐기, 부분 업로드 삭제, 수신 이미지 접근권한·보존기간·감사 로그 결정 |
| TLS | Windows 체인·호스트명 검증 유지. 인증서 배포/교체는 launcher 책임. plaintext/redirect/Control credential 재사용 금지 |

현재 전송 구현은 명시적 authorization header를 가진 방식만 지원한다.
upload_id 단독 capability 방식을 채택한다면 별도 검토·구현이 필요하다.
HTTP 성공 status만으로 PNG 검증을 증명할 수 없으므로 수신 서비스의 위 완료 의미가 필수다.

## 이번 제품 기반 구현

- `gui_product.*`: main의 Control 진입 경로에서 사용하는 owner. 서비스 미제공 시 관리 모드임을
  출력하며 GUI capability를 광고하지 않는다. 서비스가 있으면 binding/config/credential 검증 →
  실제 Windows backend와 GuiSession 생성 → probe/Host 준비 콜백 → Control 연결 순서로 소유한다.
  테스트 backend는 이 제품 owner에서 거부한다. 현재 Host용 서비스를 만드는 factory/JSON parser는 없다.
- `GuiProductServices`: 내부 서비스 주입 경계다. verified grant lookup, probe 준비,
  upload grant lookup을 받는다. 계약 미확정 부분을 true나 테스트 서버로 구현하지 않는다.
  prepare는 stop HANDLE과 30초 deadline을 지켜야 하며, authorize/grant lookup은 빠르게 반환해야 한다.
  콜백이 계약을 위반해 멈추면 강제 중단은 보장할 수 없고 Host 프로세스/VM 감독이 필요하다.
- `transport/observation/https_uploader.*`: 실제 WinHTTP async HTTPS PUT.
  승인 origin·고정 경로·scope·credential 만료·bytes/pixels·PNG signature·1회 사용을 검증한다.
  PNG signature 확인은 PNG 전체 디코딩이 아니다. 후자는 Host 수신기 책임이다.
  전체 전송은 설정 deadline(최대 30초)과 grant/credential 만료의 최소값으로 제한한다.
  20ms 취소 확인, 64 KiB 단위 전송, 16 KiB 응답 헤더 상한, 65,536개 소비 ID 상한을 둔다.
  redirect/cookie/자동 HTTP 인증을 끄며 URL·토큰·응답 본문을 진단에 출력하지 않는다.
  Windows TLS 검증은 필수이며 추가 leaf pin은 이 HTTP 모듈에서 아직 지원하지 않는다.
- 실패한 업로드는 기존 GuiSession의 block 경로로 전달된다. 이후 입력은 거부하고
  관찰 ID를 성공 응답으로 공개하지 않는다. 종료 시 GUI/worker/uploader가 owner보다 먼저 정리된다.

이 기반은 필요한 Host adapter와 수신기를 **대체하지 않는다**. CLI의 관리 모드를 일반 GUI 모드라고
부르지 않으며, 승인/업로드 계약이 없는 상태에서 실제 GUI 성공을 만들기 위한 실행 옵션도 없다.

## 제품 구성과 실행 제안

**향후 제품 구성요소**는 신뢰된 launcher/Host 통합 adapter, 업로드 grant 등록부와 HTTPS 수신 서비스다.
테스트용 loopback 서버·fake 승인·테스트 전용 EXE는 이 구성에 포함되지 않는다.

1. Launcher가 Host 인증 세션과 Guest 로컬 workspace를 준비하고, Host 정책/필수 모니터와 네트워크
   검증을 수행한다. SAN 인증서/CA 신뢰를 Guest에 배포하고 만료되는 bootstrap을 읽기 전용으로 전달한다.
2. 제품 HTTPS 서비스가 승인된 bind 주소/port, TLS key/cert, grant 등록부, 비공개 이미지 저장소로 시작한다.
   구현과 CLI는 아직 없으므로 실제로 실행 가능한 명령을 꾸며서 제시하지 않는다.
3. Host 통합 adapter가 OBSERVE 발급 전에 grant를 등록하고 Runner에 합의된 형태로 전달한다.
   원본 Sender에는 현재 이 호출점이 없다. 별도 서비스만 켠다고 UUID와 권한이 자동으로 연결되지 않는다.
   Sender 무수정 조건에서는 정식 공개 hook/외부 계약이 추가되거나 해당 API를 갖춘 upstream 커밋이 필요하다.
   monkeypatch/프록시로 요청을 가로채는 테스트 방식을 제품 통합으로 사용하지 않는다.
4. Runner의 정식 adapter가 GuiProductServices와 준비 단계 상태 전이를 연결한다.
   기존 `sandbox_runner.exe --control-context <파일>`을 제품 실행 경로로 유지하는 것이 제안이다.
   Host bootstrap 직접 읽기 여부와 관리/GUI 모드 선택 방식은 이 단계에서 합의한다.
5. startup 통과 후 원본 Host 명령으로 OBSERVE→클릭→한글→HEARTBEAT→STATE→TERMINATE를 검증한다.
   PNG 실수신과 화면 반영, 입력 중 단절/lease/업로드 만료, 일반 EXE 여부를 증거로 남긴다.

**현재 실행 가능한 것은** 기존 Control 관리 모드와 독립 Artifact/Telemetry 모드다.
테스트 파일 없이 제품 빌드가 가능하다:

```powershell
cmake -S . -B build/product -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/product
./build/product/sandbox_runner.exe --help
./build/product/sandbox_runner.exe --control-context C:/RunnerBootstrap/control.json
```

## 완료 판정

제품 기반 빌드/경계 테스트와 실제 제품 활성화의 완료 여부를 별개로 기록한다.
원본 Host 코드 변경, 임의의 검증 flag, 테스트 업로드 서버로 완료 조건을 대신하지 않는다.
실행한 검증과 미검증 목록은 [#17 검증 기록](gui-product-validation.md)을 따른다.
