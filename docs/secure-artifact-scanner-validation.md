# Runner ↔ SecureArtifactScanner 통합 검증 (작업 4)

2026-10-30 KST. `artifact-export-v1` 그대로인 Runner가, Host가 내부 검사를 SecureArtifactScanner Windows 서비스(Microsoft Defender)로 바꾼 뒤에도 끝까지 반출되는지 확인했다. **Runner 소스는 바꾸지 않았다**: 통신·계약 문제를 찾지 못했다.

요약:

- 실제 Windows Sandbox E2E는 **수행하지 못했다**. 이 PC의 Sandbox Guest에서 Smart App Control이 켜져 서명 없는 EXE를 모두 차단한다(10월 3일에 Sandbox에서 실행됐던 이전 제품 EXE도 지금은 차단). Runner 코드 문제가 아니며 우회하지 않았다.
- 대신 **같은 Runner EXE를 Host PC에서 직접 실행**해 실제 Host·실제 SecureArtifactScanner·실제 Defender와 전 구간을 검증했다: 17개 시나리오 모두 통과.
- 저장소 CMake에는 테스트 타깃이 없어 CTest는 "No tests were found"다(통과로 세지 않음).

## 환경

| 항목 | 값 |
| --- | --- |
| Host | Windows 11 Pro `10.0.26200`, Smart App Control 꺼짐, 12 논리 코어 |
| Windows Sandbox | 앱 `0.8.107.0`, Guest `10.0.26100.9711`(`WDAGUtilityAccount`) |
| Runner | `develop` `9142102`, MSYS2 UCRT64 GCC/Ninja/JsonCpp Release, `sandbox_runner.exe` 4,333,282 bytes, SHA-256 `a7ca9407f2993005e7fdecc46683e9a05d17c1dca3dd8662e82eb123c4b6d752` |
| Host | `b-hamo/host_control` `feat/secure-artifact-scanner-service` `9fa3600`, `--artifact-export --artifact-scanner defender-service` |
| Scanner Service | SecureArtifactScanner `0.1.0`, `NT SERVICE\SecureArtifactScanner`(관리자 아님), Defender 플랫폼 `4.18.26100.4`, 서명 `1.463.31.0` |
| Export 경로 | 서비스 설치 시 `-ExportRoot`로 정한 폴더 = Host `--export-dir` |

## 빌드와 CTest

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build                       # 36/36, 경고·오류 없음
./build/sandbox_runner.exe --help         # 정상
ctest --test-dir build -C Release --output-on-failure    # No tests were found!!!
```

공유 CMake가 테스트를 참조하지 않는 것이 팀 방침(`tests/`는 Git 제외)이라 CTest 대상이 없다. Runner 단위 회귀는 이번에 수행하지 않았고, 아래 실제 실행 회귀(GUI·Telemetry·전송·종료)로 대신했다.

## 실제 Windows Sandbox E2E: 차단됨

Host(`mcp_server.py`와 같은 `HostBackend`)가 Sandbox Manager로 Sandbox를 띄웠다: Sandbox 시작 3.1 s, 네트워크 확인 5.3 s, bootstrap 게시 9.6 s. 그러나 Runner가 240 s 안에 접속하지 않아 `RUNTIME_START_FAILED`(Sandbox Manager `TIMEOUT`)로 끝났다.

원인을 별도 Sandbox에서 확인했다:

| 확인 | 결과 |
| --- | --- |
| Guest `UsermodeCodeIntegrityPolicyEnforcementStatus` | `2` (강제) |
| Guest `CI\Policy\VerifiedAndReputablePolicyState` | `1` (Smart App Control 켜짐) |
| 새 빌드 `--help` (매핑 폴더 / Guest 로컬 복사) | 둘 다 "An Application Control policy has blocked this file" |
| 10월 3일 제품 EXE(`b8f40134…`, 당시 Sandbox에서 실행됨) | 동일하게 차단 |
| CodeIntegrity 이벤트 | 3077 / 3033: "did not meet the Enterprise signing level requirements", Policy `{0283ac0f-fff1-49ae-ada1-8a933130cad6}` |
| Host | `VerifiedAndReputablePolicyState = 0`, 사용자 모드 CI 강제 아님 |

Sandbox 앱 버전은 10월 3일과 같다(0.8.107.0). Guest 이미지(OS 업데이트) 쪽에서 Smart App Control이 켜진 상태가 됐다. Smart App Control은 평판 또는 신뢰된 코드 서명을 요구하므로 **Runner EXE(그리고 SecureArtifactScanner EXE)의 코드 서명**이 있어야 이 환경의 Sandbox에서 실행된다. Guest의 보안 설정을 끄는 우회는 하지 않았다. 수정 대상은 Runner 코드가 아니라 배포(서명) 결정이다.

## 대체 검증: 같은 Runner EXE를 Host PC에서 실행

Runner를 `--host-bootstrap <bootstrap.json> --host-address 127.0.0.1`로 Host PC에서 실행했다. Runner의 TLS 검사(Windows 체인·호스트명·bootstrap leaf pin)는 그대로이므로, 이 실행만을 위해 새로 만든 127.0.0.1 Host 인증서를 `CurrentUser\Root`에 사용자 확인을 받고 넣었다가 끝에서 지웠다(`trust_removed: true`). Output은 Host의 `C:\RunnerWorkspace\Sessions\session-<id>\runtime-RT-SBX-001\generation-1\output`이며, 파일은 다른 곳에 다 쓴 뒤 옮겨 넣었다(Guest 스크립트와 같은 방식). 승인은 테스트 승인자가 Host 승인 창 자리에서 답했다(`deny`가 든 이름만 거부).

다른 점: Sandbox 격리·Guest 방화벽·Sandbox Manager 수명(시작·종료·정리)은 이 방식으로 검증되지 않는다. Runner 바이너리, SCRP·Telemetry·HTTPS 경로, Host·서비스 코드는 운영과 같다.

### 결과 (17/17 통과)

증거: `build/secure-scanner-e2e-20261030-162155/evidence.json` (Git 제외. token·개인 키는 지움)

| 시나리오 | 결과 | 근거 |
| --- | --- | --- |
| Runner 연결·READY | 통과 | 실행 후 2.3 s READY, Health OK |
| GUI 회귀: 캡처·입력·Heartbeat | 통과 | `computer_observe` 1920×1080, `computer_move`(현재 커서 위치) `SUCCESS`, Health OK |
| 정상 TXT 반출 ×3 (연속) | 통과 | 각 4.6–4.8 s, `EXPORTED`, 바이트·SHA-256 일치, 서비스 Job `CLEAN`(MpCmdRun exit 0, 탐지 이벤트 0) → `DISPOSED` |
| 한글 파일명 `보고서 결과.txt` | 통과 | 같은 이름으로 공개, 바이트·해시 일치 |
| 빈 파일 | 통과 | 0 bytes `EXPORTED` |
| 1 MiB TXT | 통과 | 4.6 s, 바이트·해시 일치 |
| EICAR | 통과 (차단) | `BLOCKED MALWARE_DETECTED`, 서비스 판정 `DETECTED`: MpCmdRun **exit 0** + 탐지 이벤트 2건 → 종료 코드만으로 판정하지 않음 |
| `.exe` / UTF-8 아님 | 통과 (차단) | `FORMAT_NOT_ALLOWED` / `NOT_UTF8`, Defender 전에 형식 정책 |
| 사용자 승인 거부 | 통과 (차단) | `POLICY_DENIED`, upload grant·PUT·서비스 Job 없음 |
| 후보 보고 후 Output에서 삭제 | 통과 (차단) | Runner `CANDIDATE_UNAVAILABLE`, PUT 없음 |
| 후보 보고 후 변경 | 통과 | 옛 후보 `CANDIDATE_UNAVAILABLE`, 새 후보는 새 내용으로 `EXPORTED` |
| 검사 Timeout (Host 제한 0.05 s) | 통과 (차단) | `SCAN_TIMEOUT`, 서비스 Job 취소 후 폐기 |
| Scanner Service 연결 실패 | 통과 (차단) | 승인 창 전에 `SCANNER_UNAVAILABLE`, 요청·PUT 없음 |
| 검사 중 세션 종료 | 통과 | 종료 시점 `SCANNING`. 서비스는 `CLEAN`까지 갔지만 Job `CANCELLED` → 늦은 성공으로 반출되지 않음, 공개 파일 0, Runner exit 0 |
| Host 종료 후 새 Host·새 Runner로 반출 | 통과 | 두 번째 세션 READY 후 `EXPORTED` |
| 정리 | 통과 | 세션마다 Runner exit 0, 서비스 Job 전부 `DISPOSED`, 격리 예약 0 bytes, 시험 인증서 신뢰 제거 |

Telemetry: 모든 후보가 Host에 STORED로 등록되고(`artifact_list`) 중복 보고 없이 처리됐다. HTTPS 계약(Bearer, Content-Length, 빈 201)과 `ARTIFACT_RESULT` 대조는 변경 없이 동작했다.

## 이번 실제 실행에서 다시 하지 않은 실패 시나리오

다음은 Runner 실물로 다시 돌리지 않았다. 각각 아래 기존 검증이 있다.

| 시나리오 | 기존 검증 |
| --- | --- |
| Upload token 만료·재사용, grant 상한 초과, HTTP 403, 활성 업로드 중 두 번째 요청, 업로드 중 TERMINATE | Runner: [Sandbox 검증(10월 3일)](artifact-sandbox-validation.md) (`UPLOAD_EXPIRED`, `ARTIFACT_BLOCKED`, `UPLOAD_FAILED`, `ARTIFACT_BUSY`, `SESSION_TERMINATED`) |
| 잘못된 Session·Generation, `ARTIFACT_RESULT` 불일치, HTTPS 업로드 중단, Telemetry 단절, 파이프 단절, 공개 중 취소, SHA-256 불일치 | Host: `host_control` `tests/test_artifact_scanner_service.py`, `test_scanner_security.py`(테스트 Runner + 프로세스 안 서비스) |
| Scanner Service 강제 종료·재시작 | Host: `tests/test_scanner_integration.py` 관리자 시험(설치된 서비스) |

## 발견한 문제

| 문제 | 영향 | 수정 대상 |
| --- | --- | --- |
| Sandbox Guest의 Smart App Control이 서명 없는 EXE를 차단 | 실제 Sandbox에서 Runner가 실행되지 않음 → Sandbox 경로 전체 불가 | 배포: Runner EXE 코드 서명(신뢰된 인증서 또는 Trusted Signing). Runner 코드 수정 아님. Host의 SecureArtifactScanner EXE도 같은 조건의 PC에서는 서명 필요 |
| Host 모드 bootstrap 파일이 READY 뒤에도 남음(1회용 token, 이미 사용됨) | Host-native 시험에서만 해당. Sandbox 모드는 Sandbox Manager가 지움 | 참고: `host_control` (이번 범위 밖, 변경 안 함) |

통신·계약·보안 검증 문제는 찾지 못했다. Runner 소스 변경 없음.

## 재현

로컬 fixture(`tests/`, Git 제외, 팀 방침): `tests/secure_scanner_sandbox_e2e.py`. `host_control`의 venv로 실행한다.

```powershell
# 실제 Windows Sandbox (Runner 실행 가능한 Guest일 때)
<host_control>\.venv\Scripts\python.exe tests\secure_scanner_sandbox_e2e.py --host-control <host_control>
# Host PC에서 Runner 실행 (시험 인증서 신뢰를 잠시 추가·제거, Windows가 확인을 묻는다)
<host_control>\.venv\Scripts\python.exe tests\secure_scanner_sandbox_e2e.py --host-control <host_control> --host-native --trust-cert
```

전제: SecureArtifactScanner 설치·실행(`installer\install.ps1 -ExportRoot <폴더>`), Sandbox Manager 방화벽 규칙(17443/17444), 포트가 비어 있음, 다른 Sandbox 없음.

## 미검증

- 실제 Windows Sandbox에서의 Runner 연결·반출 전체(위 차단 때문)와 Sandbox 종료·정리 경로
- Runner CTest(대상 없음)
- 서명된 EXE로 Smart App Control Guest에서 실행
