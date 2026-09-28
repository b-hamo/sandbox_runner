# GUI 제품 연결 기반 검증 (#17)

## 범위와 상태

2026-09-28, 중단 커밋 `40661b6`에서 이어서 검증했다.
제품 lifecycle owner와 HTTPS 전송 기반의 빌드·검증은 완료했지만,
**일반 sandbox_runner.exe의 GUI 활성화와 실제 Sandbox GUI 왕복은 미완료**다.
미합의 사항은 [Host 계약안](gui-product-contract-proposal.md)을 따른다.

중단 커밋은 `gui_product.h`가 HANDLE/DWORD 선언에 필요한 헤더를 직접 포함하지 않아
단독 컴파일에 실패했다. 제품 Control 인터페이스 헤더를 포함하여 수정했다.
또한 시작 캡처 후 이미 30초 기한이 지났다면 Host prepare callback을 호출하기 전에 거부한다.

## 실행한 검증

- Host Windows에서 MSYS2 UCRT64 GCC 16.2.0, CMake/Ninja Release 제품 빌드 성공.
- 일반 PowerShell에서 제품 `--help` 성공. 관리 모드와 Host adapter 미설정 상태를 명시한다.
- 제품 EXE는 약 3.90 MiB. PE import는 Windows 기본 DLL만 사용하며 MinGW DLL 추가 없음.
- CTest 12/12 통과: 기존 Artifact/Telemetry/Control/GUI 회귀 11개와 새 observation 경계 검증.
  새 검증은 잘못된 HTTPS origin, 완료 status, timeout, 세션 세대/connection/task/action scope,
  upload ID, 크기·픽셀 상한, 만료, 헤더 주입, PNG signature, 취소 및 제품 사전 조건 거부를 확인한다.
- 실제 WinHTTP async HTTPS PUT 12개 시나리오 통과. 테스트 서버는 Host loopback에만 바인딩한다.

| HTTPS 시나리오 | 확인 결과 |
| --- | --- |
| 정확히 합의된 200 / 201 / 204 | 각각 완료로 수용, 약 197 KB PNG를 여러 chunk로 전송, 수신 바이트 원본 일치 |
| 202 / 403 | 성공으로 처리하지 않음 |
| 302 redirect | 이동하지 않고 실패, 추가 HTTP 요청 없음 |
| 응답 전 연결 단절 | 실패, 성공으로 추정하지 않음 |
| 200ms 후 취소 | 약 229ms에 반환 |
| 300ms 전체 timeout | 약 309ms에 반환 |
| 300ms grant 만료 | 약 310ms에 반환 |
| 신뢰하지 않는 인증서 / hostname 불일치 | HTTP 본문 전송 전 TLS 실패 |
| 위 모든 전송 후 같은 grant 재사용 | 거부, 두 번째 업로드 없음 |

기본 Windows 인증서 검증을 유지했다. loopback 테스트용 임시 인증서만 CurrentUser Root에
추가했고 finally에서 해당 thumbprint를 제거했다. Control token과 실제 사용자 파일은 사용하지 않았다.
첫 테스트 실행은 서버의 IPv4 바인딩과 localhost의 IPv6 우선 연결로 인해 취소가 연결 이전에 발생하여
fixture의 수신 횟수 assertion이 실패했다. IPv6 loopback 바인딩으로 fixture를 수정한 뒤
전체 12개 시나리오를 재실행하여 통과했다. 제품 timeout/취소 검사를 완화하지 않았다.

Host `feat/13-observe-wait`를 원격에서 다시 읽어 `d939215`와 이미지 수신 미구현 주석을 확인했다.
Host 소스 수정 없이 fetch/show만 수행했으며 기존 checkout의 `git status --short`는 비어 있다.

## 재현

제품은 테스트 파일 없이 빌드된다:

```powershell
cmake -S . -B build/product -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/product
./build/product/sandbox_runner.exe --help
```

이 checkout의 Git 제외 `tests/`에 observation_probe.cpp와 observation_https_test.py를 보관했다.
기존 로컬 테스트 CMake에 연결했으며 제품 CMake는 테스트를 참조하지 않는다.

```powershell
cmake -S tests -B build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
python tests/observation_https_test.py build/tests/observation_probe.exe
```

HTTPS fixture는 Python cryptography/Pillow를 사용하며 테스트 인증서가 잠시 현재 사용자
신뢰 저장소에 설치된다. 새 checkout에는 테스트 파일이 없으므로 위 로컬 테스트 명령만으로 재현할 수 없다.

## 미검증·미구현

- 실제 Sandbox에서 #17 제품 EXE의 GUI 왕복은 실행하지 않았다. 이전 #14 테스트 EXE의 성공을 재사용하지 않는다.
- 제품 Host adapter/factory, 초기 정책·lease·준비 단계 승인 전달, 실제 PNG 업로드 권한 발급과 Host 수신기는 없다.
- 최신 Host는 HELLO_ACK 이후 STATE/OBSERVE로 startup을 확인하지만 현재 GuiSession은 사전 승인을 요구한다.
  이 순환 의존성을 합의 없이 검증 플래그 true나 첫 Action의 암묵적 승인으로 우회하지 않았다.
- 준비 callback을 실제 Host에 연결한 성공 경로, 필수 네트워크/모니터 증거, 전체 제품 startup 성능은 미검증이다.
- callback이 stop/deadline을 무시하거나 OS handle 종료 callback이 멈추는 경우 강제 종료 시간은 보장하지 않는다.
  정상 I/O 대기 취소·만료 경로의 유한 반환만 이번 테스트로 확인했다.

따라서 이 변경은 제품 연결 기반 PR이며 이슈의 전체 GUI 활성화 완료를 주장하지 않는다.
