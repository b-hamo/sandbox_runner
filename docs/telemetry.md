# SCRP Telemetry 전송 계층

Issue #5의 일반 `SECURITY_EVENT` 전송 기반을 Issue #7에서 Runner 시작·종료에 연결했다.
제품 EXE는 외부 Session Context로 Telemetry를 시작하고 READY 확인 뒤 Artifact 감시를 시작한다.
Artifact 후보 변환·업로드·Control HELLO_ACK는 미구현이며 기존 Artifact 모듈 책임은 유지한다.

## Runner Session Context 입력과 lifecycle

`src/session_context.h`의 `runner::SessionContext`는 기존 `telemetry::Context`의 alias다.
`runner::validate_session_context()`는 세션 ID·Runtime ID·양의 generation, WSS endpoint,
TLS 설정, schema, credential provider 및 현재 credential을 검사한다. provider는 시작 전에도
호출되며 매 연결 시 Client가 다시 검증한다. 예외에 credential이 포함돼도 진단으로 노출하지 않는다.
기존 C++ `TlsTrust`의 빈 pin은 Windows 인증서 체인·호스트명 검증 사용을 뜻하며 trust 누락이 아니다.
인증서 저장소의 실제 신뢰 여부는 WSS 연결에서 검증한다.

`runner::start_telemetry(context, stop_event, ready_timeout, options, factory)`는 Client를 생성·시작하고
READY인 `unique_ptr<TelemetryClient>`를 반환한다. 기본 READY 대기는 30초이며 테스트에서 짧게
주입할 수 있다. 재시도 소진·READY 시간 초과·종료 이벤트·초기화 오류는 예외로 명시하고 생성된
Client를 stop/join한 뒤 반환한다. handshake는 기존 Client 내부에서만 수행한다.
초기화 실패 시 EXE는 비정상 종료하며 Output을 준비하거나 감시 worker를 시작하지 않는다.
현재 Ctrl+C/Ctrl+Break를 READY 대기 중 받으면 시작 취소로 비정상 종료한다.

정상 종료는 watcher I/O 종료 → CandidateDetector stop/join → Telemetry stop/join → 종료 이벤트 해제다.
생산자를 먼저 끝내고 전송 계층을 정리한다. Client 소멸자의 중복 stop도 안전하다.
Pending은 stop 후 확인 가능하며 소멸 시 메모리가 해제된다. 미확인 이벤트를 ACK 처리하지 않는다.
READY 이후 재연결은 기존 Client 정책이며 Host의 장애 대응·Coverage 연동은 후속 작업이다.

### 실행 파일의 명시적 로컬 입력

```powershell
& .\build\sandbox_runner.exe --session-context C:\session\context.json --output C:\session\Output
```

`--session-context`는 필수다. `--output` 생략 시 READY 후 기본 세션 Output을 준비한다.
입력 파일은 최대 64 KiB의 UTF-8 JSON object이며, 아래 필드는 모두 필요하다.
설정 객체의 중복·알 수 없는 key, 잘못된 타입, 누락은 실패한다. handshake payload의 필드는
외부 계약이 정한다. 파일 경로는 Unicode를 지원한다.

| 로컬 필드 | 값·검증 |
| --- | --- |
| `session_id`, `runtime_id` | 외부에서 전달된 비어 있지 않은 문자열 |
| `generation` | 0보다 큰 uint64 정수. 문자열·실수·음수 거부 |
| `endpoint` | Host의 정확한 `wss://` 주소. query·userinfo·평문 fallback 없음 |
| `credential.header_name`, `credential.header_value` | 외부에서 합의한 헤더 이름·값. 기본 헤더·토큰 없음 |
| `credential.expires_unix_seconds` | 미래 만료 시각(Unix 초 정수), clock 표현 범위 이내 |
| `tls.trust` | 명시적으로 `windows-system` 지정. 인증서 체인·호스트명 검증 필수 |
| `tls.leaf_sha256` | 빈 문자열 또는 소문자 64자리 SHA-256 추가 pin |
| `handshake.hello_payload` | 외부 계약의 CHANNEL_HELLO payload object |
| `handshake.ack_status` | 기대 CHANNEL_ACK status 문자열 또는 null |
| `handshake.ack_payload` | 기대 CHANNEL_ACK payload object 전체 |

이 파일 형식은 **로컬 주입용**이며 SCRP/Control wire 스키마를 확정하지 않는다.
인증 헤더·테스트 ID·토큰·ACK 성공값을 제품 기본값으로 제공하지 않는다.
로컬 `InjectedHandshake` adapter는 ACK의 status/payload를 외부 기대값과 정확히 비교하고
error가 null인지 검사한다. Client가 Envelope의 세션·상관 ID·connection_id 등을 추가 검증한다.
ACK payload에 동적 connection_id 등이 있으면 이 단순 adapter만으로 일반 Host 계약을 표현할 수 없다.
이 경우 향후 Control 입력에서 실제 `TelemetrySchema` 구현과 Context를 직접 주입한다.
로컬 adapter는 SECURITY_EVENT/EVENT_ACK를 지원하지 않으며 해당 API 사용을 명시적으로 거부한다.
이는 테스트용 handshake 계약이 제품 이벤트 계약으로 확장되는 것을 방지한다.

파일은 credential을 포함하므로 신뢰된 launcher가 해당 세션만 읽을 수 있게 배치하고 수명을 관리한다.
Runner는 내용을 로그로 출력하거나 trust anchor를 설치하지 않는다. CLI에는 토큰 대신 파일 경로만 준다.
현재 파일은 시작 시 한 번 읽으며 자격 갱신·Control 인증·Host discovery를 수행하지 않는다.
테스트 값은 Git 제외 `tests/`에서 생성하고 임시 파일·인증서는 검증 후 제거한다.
향후 `main.cpp`의 파일 입력을 Control HELLO_ACK → `runner::SessionContext` 변환으로 교체한다.

## 호출 계약

`telemetry::Context`에 다음을 주입한다.

- `scrp::SessionContext`: Host가 발급한 session_id, runtime_id, 양의 Runtime generation.
- `ConnectionSettings`: 정확한 WSS endpoint, TLS trust, I/O timeout, 전체 수신 한도.
- credential provider: 합의된 인증 헤더 이름·값과 만료. 매 연결 시 호출하며 자체 토큰 생성·갱신은 하지 않는다.
- `shared_ptr<const scrp::TelemetrySchema>`: CHANNEL_HELLO payload 생성, SECURITY_EVENT의 닫힌 스키마 검증,
  CHANNEL_ACK의 성공·오류·payload 검증과 connection_id 추출, EVENT_ACK 저장 성공 검증과 event_id 추출.

스키마와 provider는 빠르게 반환해야 한다. 스키마는 enqueue 호출 스레드와 worker에서 동시에
사용될 수 있으므로 thread-safe해야 하며 Client를 재진입 호출하지 않는다. factory를 생략하면
실제 WinHTTP WSS가 사용된다. factory 주입은 독립 상태 테스트용이다.

`SecurityEvent`는 event_id, observed_at, JSON payload를 가진다. payload의 event_id·observed_at은
구조체 값과 같아야 한다. category·process·relation·confidence·coverage 등 세부 필수·nullable 규칙은
합의된 Schema가 검증한다. payload의 action_id가 있으면 Envelope action_id에도 반영한다.
클라이언트는 enqueue 시 복사하며 연결이 바뀌어도 원래 이벤트를 수정하지 않는다.

```cpp
// context는 실제 Host 계약 구현과 credential provider가 채운다.
telemetry::TelemetryClient client(context);
auto result = client.enqueue(event); // start 전에도 가능, 반환값을 반드시 처리
client.start();
// 다른 이벤트 생산자는 enqueue(), 상태 소비자는 snapshot() 사용
client.stop(); // I/O 취소 및 join; 여러 번 호출 가능
```

Client는 한 번 시작한다. `start/stop`은 직렬화되고 enqueue·snapshot은 동시 호출 가능하다.
세션/generation 변경은 새 Client로 처리한다. 이전 Pending을 새 Runtime으로 자동 이전하지 않는다.
제품 생성·시작·종료는 위 Runner lifecycle에 연결되어 있으며 실제 Control 입력 변환만 후속 작업이다.

## Envelope와 ACK

공통 필드는 설계안의 version="1.0", session_id, runtime_id, generation, connection_id,
message_id, task_id, action_id, sequence_number, timestamp, nonce, type, correlation_id,
status, error, payload다. W1 문서 버전 v0.1과 wire version은 서로 다르다.

- message_id는 BCrypt 난수 기반 UUIDv4, nonce는 128-bit 난수의 unpadded base64url이다.
- CHANNEL_HELLO의 connection_id는 null, 송신 sequence는 1이다. CHANNEL_ACK의 Host connection_id에 바인딩한다.
- READY 이후에만 SECURITY_EVENT를 송신한다. 단순하고 제한 가능한 형태로 한 이벤트씩 ACK를 기다린다.
- 수신 Envelope는 세션·Runtime·generation·정확한 다음 sequence·UUIDv4·nonce·UTC 시각을 검증한다.
- ACK의 correlation_id는 HELLO 또는 현재 이벤트 전송 message_id와 같아야 한다.
  EVENT_ACK는 현재 connection과 in-flight event_id까지 같아야 Pending을 제거한다.
- 알 수 없는 메시지, 오류 상태, 미송신/알 수 없는 이벤트, 이전 연결 ACK는 정상 ACK로 처리하지 않는다.
  연결을 닫고 Pending을 유지한다. 상태/payload의 성공 판정은 Schema에 위임한다.
- 수신 message_id·nonce 재사용을 거부한다. 캐시는 연결당 최대 65,536 메시지로 제한하며
  한도 도달 시 연결을 새로 만든다. 기본 타임스탬프 허용 오차는 60초다.
- UTF-8, 중복 JSON key, 알려지지 않은 Envelope 필드, 잘못된 타입, non-finite 숫자,
  과도한 깊이(16), 추가 JSON 데이터를 거부한다. UTC RFC3339는 초 또는 밀리초 정규 형식을 지원한다.

재전송은 같은 event_id·observed_at·payload에 새로운 message_id·nonce·전송 시각·연결별 sequence를
부여한다. 파일 변경 세대와 Runtime generation은 혼용하지 않는다. Host는 event_id로 중복 저장을
방지해야 한다. ACK는 저장 확인이며 Artifact 승인·안전 판정이 아니다.

## 제한·장애·종료

`ClientOptions`에서 초깃값을 줄여 테스트/배포 정책에 맞출 수 있다. 현재 구현은 설계안 상한을 넘겨
설정하지 못하게 한다. 해당 수치가 팀의 영구 동결값이라는 뜻은 아니다.

| 항목 | 현재 기본값·동작 |
| --- | --- |
| 이벤트 | Envelope 포함 최대 16 KiB |
| Pending | 합계 최대 10 MiB, 최초 enqueue 후 최대 5분 |
| 수신 메시지 | 재조립 완료 전부터 최대 64 KiB, binary 거부 |
| HELLO ACK | 5초 |
| EVENT ACK | 2초 |
| 네트워크 I/O·분할 메시지 | 5초, monotonic clock 기반 대기 |
| 재연결 | 1·2·4·8초 + 최대 25% jitter, 이후 DISCONNECTED와 오류 노출 |

이벤트의 enqueue 바이트는 재전송 시의 최대 sequence 길이와 connection JSON 문자열 258바이트를
예약해서 계산한다. 따라서 실제 작은 connection_id 사용 시에도 16 KiB보다 약간 작은 payload부터
거절될 수 있다. connection_id의 JSON 표현은 이 상한 이내여야 한다. 바이트 예산은 직렬화 기준이며
JSON 객체·컨테이너의 allocator overhead까지 정확히 10 MiB로 제한한다는 뜻은 아니다.

`enqueue()`는 accepted/duplicate/conflict/event_too_large/buffer_full/invalid/stopped를 반환한다.
동일 ID·동일 payload는 Pending 중 중복으로 처리하고 같은 ID·다른 payload는 거절한다.
ACK 완료 후에는 Host의 event_id 중복 제거가 최종 기준이다.

`snapshot()`은 Pending 개수·바이트와 거부·만료·오류 누계, 마지막 진단을 제공한다. 만료된 이벤트는
명시적으로 수를 기록한 뒤 제거한다. 초과 시 기존 항목을 밀어내지 않고 새 enqueue를 거절한다.
소비자는 이 결과를 Coverage 저하로 처리해야 한다. Host Action 차단이나 Coverage 전송 스키마를
이 계층에서 임의 구현하지 않는다. 외부 계약 예외 메시지와 peer payload·credential은 로그로 복사하지 않는다.

저장은 메모리 전용이다. `stop()`은 네트워크 대기를 취소하고 WinHTTP 마지막 HANDLE_CLOSING
콜백까지 회수한 후 worker를 join한다. Pending은 Client 생존 중 snapshot으로 확인할 수 있으며,
미확인 이벤트가 남으면 오류로 노출한다. 종료 시 ACK를 기다리며 무제한 drain하지 않는다.
프로세스 재시작에 걸친 영속성이나 강제 종료 시 전달 보장은 없다.

## TLS·인증

WinHTTP의 기본 Windows 인증서 체인·호스트명 검증을 유지하고 최소 TLS 1.2를 지정한다.
선택적인 leaf SHA-256 pin은 추가 조건이다. self-signed 인증서를 pin만으로 신뢰하지 않는다.
Bootstrap 신뢰 앵커를 Guest Windows 저장소에 배포하는 작업은 Runtime 담당이다.
임의 CA 파일을 자동 설치하거나 인증서 검증을 끄는 기능은 없다.

인증 헤더 형식을 제품 코드에서 정하지 않는다. 외부에서 주입된 nonempty 헤더·값·만료를 검증하고
WSS upgrade에만 사용한다. 헤더 개행 삽입과 WebSocket/Host 헤더 덮어쓰기를 거부한다.
HTTP redirect, cookie, 자동 HTTP 인증, proxy 자동 탐색, ws:// fallback, 압축 협상은 사용하지 않는다.
endpoint는 신뢰된 Host 설정이어야 한다. URL query·fragment·userinfo를 허용하지 않는다.

WinHTTP 구현 참고: [비동기 동시성·취소](https://learn.microsoft.com/en-us/windows/win32/winhttp/concurrency-in-winhttp),
[핸들 종료와 콜백 수명](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle),
[WebSocket upgrade](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpwebsocketcompleteupgrade).

## 제품 빌드와 검증 이력

Host MSYS2 UCRT64에 GCC·CMake·Ninja·JsonCpp가 필요하다. 확인 환경은 GCC 16.2.0,
JsonCpp 1.9.8이며 이 버전을 팀 표준으로 고정하지 않았다. CMake의 C++ 표준 설정도 기존 정책을 유지한다.
`jsoncpp_static`, Windows 기본 WinHTTP·BCrypt·Crypt32를 연결한다. 별도 JSON/WebSocket DLL은 필요하지 않다.

```bash
pacman -S mingw-w64-ucrt-x86_64-jsoncpp
cmake -S . -B build/telemetry -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/telemetry
```

테스트 소스·스크립트·테스트용 CMake는 로컬 `tests/`에만 보관하고 Git에서 제외한다.
공유 CMake에는 테스트 타깃·옵션이 없으며 제품 빌드는 테스트 파일이나 Python을 요구하지 않는다.
테스트용 인증 헤더·payload는 로컬 검증에만 사용했으며 Host SCRP 계약을 확정하는 값이 아니다.
아래 결과는 테스트를 저장소에서 제외하기 전에 수행한 검증 이력이다.

2026-09-27 Host 검증: UCRT64 Release 빌드, 기존 Output/Candidate 테스트, Telemetry 단위 테스트 및
실제 WSS의 ACK 전 끊김·재전송·분할 ACK·신뢰되지 않은 인증서·잘못된 호스트명/pin·잘못된 ACK·binary/
과대 수신·timeout·redirect 거부·idle/handshake 중 종료를 통과했다. 임시 trust 제거를 확인했다.
실제 Windows Sandbox의 네트워크·신뢰 앵커 배포 및 팀 Host Security Backend와의 호환성은 미검증이다.

MSYS2를 PATH에서 제외한 PowerShell에서 Runner `--help`와 Telemetry 단위 테스트를 실행했다.
`objdump -p`의 의존성은 Windows 기본 DLL만 포함했다. strip 전 새 EXE는 3,485,738바이트이며,
기존 로컬 `build/sandbox_runner.exe`는 2,833,295바이트였다. 같은 Host에서 `--help` 5회 실행은
새 EXE 약 12~15ms, 기존 로컬 EXE 약 11~41ms였다. 이는 시작 비용 확인용 로컬 측정이며
실제 Sandbox 시작이나 네트워크 handshake 성능 측정은 아니다. 테스트 프로세스와 임시 Root 인증서 잔존은 없었다.

### Issue #7 검증 결과 (2026-09-27)

UCRT64 Release 제품 빌드와 로컬 CTest **6/6**이 통과했다(최종 재검증 59.82초).
`output_watcher`, `artifact_candidate`, `telemetry`, `telemetry_wss` 회귀 및
`runner_lifecycle`, `runner_wss` 통합 검증을 포함한다.

- Context 필수 값·generation 타입·TLS trust·credential·만료·JSON 오류의 명시적 실패.
- 주입 Context의 ID·generation 전달, READY 반환, 연결 실패·CHANNEL_ACK 시간 초과·Runner READY 제한·시작 중 취소.
- 반복 start/stop 25회 후 핸들 수 증가 없음, socket 소멸·worker 종료, 중복 stop·종료 후 enqueue 거부.
- 실제 EXE의 WSS CHANNEL_HELLO/ACK → READY → Artifact 후보 → Ctrl+C 정상 종료를 3회 확인.
  peer에서 socket 종료를 확인했으며 Artifact SECURITY_EVENT는 전송하지 않았다.
- 실제 EXE 연결 실패 시 감시 시작 차단. 실제 WSS 회귀의 TLS 거부·재전송·I/O 취소 유지.
- 임시 인증서와 테스트 프로세스 잔존 없음. PowerShell에서 제품 `--help` 실행 확인.
  EXE는 strip 전 3,513,414바이트이며 DLL 의존성은 Windows 기본 DLL뿐이다.
  MSYS2를 PATH에서 제외한 도움말 5회 실행은 136.6/13.7/11.8/13.2/12.3ms였다.
  이는 Host 로컬 실행 측정이며 실제 Sandbox 시작·handshake 성능 측정이 아니다.

테스트는 기존 정책대로 Git 제외 `tests/`에 있다. symlink 하위 검사는 권한 부족(Win32 1314)으로
건너뛰었으며 junction 검사는 통과했다. 이 작업의 실제 Sandbox·팀 Host 호환성은 미검증이다.
한글 절대 경로가 오브젝트 경로에 중복되는 로컬 GCC assembler 문제는 아래 경로 해시 옵션으로 회피했다.

```bash
cmake -S tests -B build/local-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OBJECT_PATH_MAX=190 -DRUNNER_TEST_LOCAL_TLS=ON -DPython3_EXECUTABLE=<Windows-python.exe>
cmake --build build/local-tests
ctest --test-dir build/local-tests --output-on-failure
```

Python은 `cryptography`가 필요하다. WSS 테스트는 매번 생성한 테스트 인증서만 현재 사용자 Root에
잠시 등록하고 finally에서 해당 thumbprint를 제거한다. 제품은 인증서를 설치하지 않는다.
실제 Sandbox 재현은 README의 절차대로 Host가 배포한 신뢰 앵커·Context로 실행하고
READY·후보 로그·Ctrl+C 종료를 확인한다. Sandbox의 localhost를 Host 주소로 쓰지 않는다.

## 남은 합의·연결

1. CHANNEL_HELLO/ACK, EVENT_ACK, category별 SECURITY_EVENT의 필수/nullable·닫힌 스키마 및 ACK status.
2. Control HELLO_ACK에서 전달되는 Telemetry 자격의 정확한 헤더·Scope·만료·재발급 계약.
3. Bootstrap TLS 신뢰 앵커 배포, Windows 저장소 외 private CA 지원 필요 여부.
4. 로컬 Context 입력을 실제 Control HELLO_ACK 기반 provider·Schema로 교체하고 상태·Coverage를 Host 정책에 연결.
5. 후속 Artifact Publisher에서 후보/무효화 이벤트를 변환. CandidateDetector 내부에 네트워크 책임을 넣지 않는다.
