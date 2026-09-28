# 실제 Windows Sandbox Host Sender 검증 (#14)

2026-09-28 11:51 KST, 실제 Windows Sandbox(Guest Windows 10.0.26100.0,
WDAGUtilityAccount, Virtual Machine)에서 실행했다.

## 결과와 적용 범위

**실제 GUI 모듈의 Host 왕복은 성공했다. 기본 제품 CLI의 GUI 활성화는 아직 미완료다.**

| 대상/요청 | 실제 결과 |
| --- | --- |
| 제품 `sandbox_runner.exe --control-context` | 인증 성공, GUI capability 없음, OBSERVE에 UNSUPPORTED_TYPE, 종료 코드 1 |
| 실제 제품 모듈을 연결한 `sandbox_gui_probe.exe` | 인증 성공, gui.observe/gui.input 협상, 종료 코드 0 |
| OBSERVE | 실제 PNG 2030×1230, 35,141 bytes를 HTTPS로 수신·디코딩 |
| mouse.click | ACK → SUCCESS; Guest EDIT가 (640,420)의 클릭 1회를 기록 |
| keyboard.type | ACK → SUCCESS, chars_sent=5; EDIT 내용이 `안녕하세요`와 정확히 일치 |
| HEARTBEAT | ALIVE, READY, queue=0 |
| STATE_REQUEST | ACT-000003 SUCCESS |
| TERMINATE | TERMINATE_RESULT, 원본 Sender의 `demo complete`, Runner 종료 |

테스트 EXE는 변경 없는 제품 `runner_lifecycle`, `GuiSession`, Runtime,
GDI/WIC/SendInput, WinHTTP를 연결한다. fake backend나 메모리 WebSocket을 사용하지 않았다.
제품 CLI에 아직 없는 승인·업로드 adapter와 관찰 가능한 EDIT 창만 테스트 fixture에서 제공했다.
이 결과를 제품 진입점의 완성이나 운영 승인·모니터링 통합 성공으로 해석하면 안 된다.

## Host 원본 보존

- 저장소: https://github.com/b-hamo/host_control
- 실행 브랜치: `feat/3-auth-tls`
- 고정 commit: `973822e55196074a46fecd207f5acd16d5cde5ab`
- `host/sender.py` SHA-256: `aedd9a98897a80230f8a1f5916029642e0b07086669e463ee4caea448b68cfd0`
- 테스트 전후 모든 Git 추적 파일의 SHA-256 manifest 일치, `git status --short` 출력 없음.

기본 develop에는 Sender 구현이 없어 WSS/auth가 구현된 위 브랜치를 사용했다.
Runner 쪽 외부 QA 스크립트가 원본 `sender.start_server`와 SessionRegistry를 호출했다.
요청 생성·인증·스키마 검증·데모 순서는 원본 그대로이며 monkeypatch/프록시/소스 수정은 없다.
별도의 테스트 HTTPS 수신기는 원본 Host에 아직 없는 이미지 수신·검증 역할만 맡았다.

## 실행 조건

Host에서 MSYS2 UCRT64 GCC/CMake/Ninja로 빌드한 EXE만 읽기 전용 `.wsb` 매핑으로 배치했다.
Guest Output과 작업 디렉터리는 Guest 내부에 두었다. Host private key는 매핑하지 않았다.
Host 주소 IP SAN을 가진 임시 인증서를 Guest에서 사용자가 승인했다.
WSS/HTTPS의 체인·호스트명 검증을 유지했고 Control에는 추가 leaf pin을 적용했다.
원본 SessionRegistry의 세션별 일회성 bootstrap token으로 인증했다.

Guest 방화벽의 outbound 기본 차단과 QA Host의 TCP 17443/17444만 허용하는 규칙을 확인했다.
시작 probe는 Host에서 실제 PNG 디코딩 성공 후 승인했다. 테스트 정책의 외부 필수 모니터 목록은
명시적으로 빈 목록이며, 운영 보안 모니터 검증을 수행했다고 주장하지 않는다.
관찰 이미지는 별도 토큰·고정 경로·일회성 수신·8 MiB/16 MP 상한으로 HTTPS 전송했다.
이는 테스트 계약이며 운영 upload 권한 계약을 확정한 것이 아니다.

## 증거와 수집 오류

로컬 `build/sandbox-sender-20260928-115050/evidence/`에 Sender 로그, Guest 보고,
전후 Host source manifest, probe/observe/evidence PNG와 `verified-result.json`을 보관했다.
관찰 PNG SHA-256:
`ad6465ebc5ef2656ec00e24485d4ec26b407d77a8835b2884c295afe7b9920dc`.
입력 후 화면 SHA-256:
`1d38e16d33e1b765adf9d621690f917fb2f9fa64a3789d939b23cf645cd6705a`.
실제 화면에서도 한글 입력을 확인했다.

Guest의 마지막 JSON 파일 수집은 PowerShell 5.1의 기본 인코딩 때문에 실패했다.
독립적으로 UTF-8 수집된 EXE stdout의 JSON에는 `pass=true`, `matched=true`, `runner_exit=0`,
실제 클릭 좌표·횟수와 한글 원문이 있다. Host에서 이 JSON을 다시 파싱하여 검증했고,
저장된 PNG들의 해시와 원본 Sender의 완료 로그를 대조했다. 실패 보고를 성공으로 덮어쓰지 않았다.
로컬 Guest 스크립트의 `Get-Content`에 `-Encoding UTF8`을 추가했지만 전체 Sandbox 재실행은 하지 않았다.
테스트 Host 서버는 검증 후 정상 종료했다.

## 재현과 남은 항목

기존 방침대로 테스트 소스는 Git 제외 `tests/`에 있다:
`sandbox_gui_probe.cpp`, `sandbox_sender_host.py`, `sandbox_sender_guest.ps1`.
새 checkout만으로 이 로컬 fixture를 재현할 수는 없다.
현재 checkout에서는 UCRT64로 `build/tests`의 `sandbox_gui_probe`를 빌드하고,
외부 Host QA 스크립트에 새 run 디렉터리·원본 Host checkout·Host IP를 전달한다.
발급된 짧은 수명의 context와 EXE를 읽기 전용 매핑한 새 Sandbox에서 Guest 스크립트를 실행한다.
임시 인증서 승인 뒤 원본 데모가 자동 실행된다. bootstrap token 재사용은 불가하다.

제품 기본 CLI의 Host 승인/uploader 연결, 운영 권한 계약과 모니터링,
스크롤·hotkey·여러 DPI/UIPI 환경, 입력 중 실제 WSS 단절/lease 만료,
Artifact/Telemetry 동시 lifecycle 및 시작 성능은 이번 실제 Sandbox 테스트 범위 밖이다.
