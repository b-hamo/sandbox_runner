# 앱 창 표면 (#37)

`--host-bootstrap <파일> --host-address 127.0.0.1 --surface app-window`로 명시적으로 선택한다.
기본 desktop 표면은 그대로다. HostAppWindow 시작 권한은 실제 AppWindowBackend와 정상 Output 감시가 있어야 READY가 된다.
TLS·인증·정책·세대·작업 큐·중복 방지·artifact HTTPS 업로드는 기존 구현을 사용한다.

전용 Win+R 입력 창에서 시작한 프로세스를 자신의 Job에 넣은 뒤 실행한다.
관찰 대상은 해당 Job의 보이는 최상위 창이며 HWND·PID·프로세스 핸들을 검증한다.
PrintWindow의 실제 PNG를 반환하며 2.5초 제한과 동시 1개 캡처 제한을 둔다.
앱 종료 후에는 실제 런처 창을 새로 관찰한다. 앱 화면을 재사용하지 않는다.
입력은 화면 좌표를 해당 창의 자식 컨트롤로 변환하고 Unicode WM_CHAR로 전달한다.
실행 게이트·취소·시간 제한을 글자마다 확인한다. 부분 전달은 재전송하지 않는다.

Manager가 준비한 세션별 읽기 전용 `builtin-apps/notepad`가 Runner 옆에 있으면 자신의 PATH에
추가한다. 전용 실행창과 자식 프로세스는 `notepad`/`notepad.exe`로 이 사본을 찾을 수 있다.
Guest 준비 때의 파일 해시 검사는 Manager가 담당하며 Host Windows 폴더를 공유하지 않는다.
프로그램 시작 실패는 일반 입력 거부와 구분해 내부 실행 오류 및 Win32 오류 번호를 남긴다.
실행 응답 시간 초과는 UNKNOWN으로 남겨 입력이 전달되지 않았다고 단정하거나 재전송하지 않는다.
2026-11-10 빌드 SHA-256 `07493a84a8eab91d60c398d398bab4cd4be9ec51687f43fa810884d0a0318c75`로
실제 micro_VM에서 `notepad.exe` 실행 및 한글/영문 입력 화면을 확인했다.
증거는 `C:\SCRPAppLab\notepad-auto2-20261110\15-obs-0.png`다.
당시에는 메모장 저장·반출과 일반 채팅 기본 배포 반영을 검증하지 않았다. 2026-11-11 결과는 아래 #43 절을 참조한다.

지원 범위: Win32/WinForms 창의 왼쪽 클릭, 이동, Unicode 텍스트, 일부 제어 키와 아래 메뉴/편집 단축키,
전용 Win+R 실행 창. 전체 바탕화면, OS Run 창, 모든 단축키·앱·GPU 창·IME 조합을 지원한다고 주장하지 않는다.
Job에 kill-on-close를 설정하지 않으므로 Runner 연결 종료만으로 앱을 죽이지 않는다.
앱 Job 재확보와 Manager/Host의 Runner 재시작 연결은 아래 후속 수정에 포함된다. 실제 프로세스 종료를 넣는 종단 검증 결과는 문서 끝에 기록한다.
파일 반출을 사용하는 Host GUI 프로필은 전송 오류 때 HELLO_ACK에서 받은 미사용 reconnect 자격으로
새 연결을 시도한다(최대 4회, 각 성공한 연결에서 새 자격을 받은 경우에만 다음 시도 가능).
동일 Runner의 Runtime·앱 Job·action 기록은 유지하고, 이전 연결의 대기 작업·관찰 기준·미전달 결과는 버린다.
새 HELLO_ACK와 lease, Host의 재동기화 전에는 입력을 허용하지 않는다. 토큰을 재사용하거나 입력을 재전송하지 않는다.
프로토콜·정책 오류, 명시적 종료, 인증 만료는 이 재연결 대상이 아니다.
2026-11-08 실제 micro_VM 중계 장애에서 같은 VM·세대의 화면/한글 유지와 재연결 후 입력·검사 반출을 확인했다.
Host GUI 루프는 이전 파일 작업·업로더·Telemetry를 중단하고 join한 뒤에만 새 HELLO를 보낸다.
이 보장은 `artifact.reconnect.v1`으로 협상하며 Host PR #115와 함께 배포해야 한다.
Host는 새 Control 연결에 묶인 Telemetry 자격을 검사하고 유휴 Output에서만 재개한다.
진행 중이거나 해제가 확인되지 않은 snapshot은 계속 차단한다.
`verification/reconnect_ledger_test.cpp`는 capability opt-in·중복·필수 반출 capability와 함께
중복/상충 입력, 이전 connection 거부, 전송/프로토콜 실패 분류를 대역으로 검사하며 실제 VM 연결 시험과 구분한다.
프로세스 시작 ACK는 완료 증거가 아니며 앱 화면과 검사된 Router 영수증으로 확인한다.
진단 stdout/stderr는 Guest C:\RunnerLogs에만 쓰며 명시된 로그/NUL 핸들만 자식에게 상속한다.

## 2026-11-09 후속 수정

`artifact.reconnect.reset.v1`은 이전 전송 worker가 종료된 뒤 새 인증 Control 연결에서 Output snapshot
재설정을 허용하는 협상이다. 기존 reconnect/반출 capability가 함께 있어야 한다. Host는 진행 중인 수신 종료를
기다리고, 이전 upload grant를 재사용하지 않으며, 새 후보를 전부 수신·검사한다.
실제 B에서 7,340,069바이트 중 61,324바이트 수신 후 연결을 끊었고 같은 VM/세대에서 재연결했다.
불완전 후보는 취소 상태로 유지했고 새 후보를 SHA 일치 및 Defender 검사 후 반출했다. 기존 앱의 한글도 유지됐다.

앱 Job은 세션 이름에 묶고 별도 controller event로 중복 제어자를 거부한다. 이름만 지정하면 마지막 Job 핸들이
닫힌 뒤 OpenJobObject가 실패하므로, 동일 executable의 내부 `--app-job-keeper` 모드가 Job QUERY 핸들을 유지한다.
상속 핸들은 Job 조회, 준비 event 통지, 원래 Runner의 종료 조회 세 개로 제한한다. keeper는 앱을 종료하지 않으며
원래 Runner가 종료되고 Job 안에 앱도 없어지면 끝난다. PID 재조회나 controller event 유지로 다른 제어자를 막지 않는다.

`verification/app_ownership_test.cpp`를 실제 WSB A에서 실행해 제어 프로세스 종료 후 앱 생존·동일 Job 재확보·한글 입력·
다른 세션의 앱 미획득을 확인했다. SCRP RUN-0006의 GUEST_PROCESS_VERIFIED/exit 0과 검사 반출된 stdout을 보존했다.
이것은 구성요소 시험이며 실제 B Runner를 종료하고 Host가 새 인증 세대로 재시작한 종단 시험은 아니다.
새 Runner 빌드 SHA는 `3e717ac9b9beff113eb208793076c923379e28de20f8517140f648cb5bccae2c`이며,
현재 B/C의 실행 바이너리 `c5fb9e5b78800612ae86a0e0e628945c303acc328b8f6a01b08521377850108c`와 구분한다.

후속 복구 빌드 `e010e6f4d22f9b0470705bb51bbb0f290eb619eb4c0c6cd405e312b99ddbdd08`는
bootstrap의 선택 필드 `output_storage_generation`을 지원한다(정수, 1 이상 현재 인증 generation 이하).
생략하면 현재 generation을 사용한다. 재시작 때 인증·실행 디렉터리는 새 generation으로 만들고,
Output만 최초 저장 generation에 고정해 살아 있는 앱이 저장하는 경로를 보존한다.
기존 Output의 모든 조상 디렉터리를 재분석 지점 없이 고정하고 삭제 공유를 허용하지 않는다.
이미 존재하는 새 실행 generation 또는 미래 저장 generation은 거부한다.
이 계약은 해당 필드를 검증하는 Host/Runner를 함께 배포해야 하며 이전 Runner에 보내지 않는다.
실제 WSB A의 RUN-0010에서 기존 Output 보존·디렉터리 이동 거부·generation 재사용 거부를 확인했다.
이 구성요소 시험 통과만으로 micro_VM Runner 종료 후 종단 복구 성공을 의미하지 않는다.

같은 후속 Runner 빌드의 실제 micro_VM 시험에서 PID 4764를 종료한 뒤 같은 VM에 PID 2488이 새 인증
generation 2로 연결됐다. 기존 앱 PID 1056과 `복구 전 작업 유지` 텍스트를 유지했고 추가 한글 입력을 확인했다.
종료 직전 Router 작업은 UNKNOWN으로 남아 재전송되지 않았고 새 Router 작업은 검증 영수증과 Defender 반출을 통과했다.
앱 저장 버튼은 입력 전달 뒤 저장 파일이 생기지 않았다. 이를 버튼 저장 성공이나 모든 앱 입력 지원으로 일반화하지 않는다.
기존 A/B/C는 READY를 유지했다. Host의 mixed-vm-hardening-20261109.md와 scrp-recovery3-20261109가 증거다.

## 2026-11-11 앱 창 단축키 (#43)

기존 `KeyboardHotkey`는 Win+R과 Ctrl+A만 처리해 메모장의 Ctrl+S를 INPUT_REJECTED로 거부했다.
`app_shortcuts.cpp`는 소유 앱의 Win32 메뉴에서 탭 뒤에 표시된 단축키를 읽고 정확한 modifier 조합을
일치시킨다. 메뉴 상태를 갱신한 뒤 accelerator 형식의 WM_COMMAND를 해당 창에 한 번 큐잉한다.
메모장의 저장 명령 번호를 하드코딩하지 않는다. 이는 Windows의
[메뉴와 accelerator 명령 처리](https://learn.microsoft.com/en-us/windows/win32/menurc/about-keyboard-accelerators)에 따른다.
큐잉 성공은 입력 전달이며 저장 성공은 화면·파일로 따로 확인한다. Save As의 modal loop 때문에
명령 완료를 SendMessageTimeout으로 기다리지 않는다. 메뉴 초기화/편집 메시지 응답이 끊기면 UNKNOWN이며 재전송하지 않는다.

- 메뉴가 노출하는 Ctrl+S, Ctrl+Shift+S 등의 조합. Ctrl/Control, Shift, Alt, Win 표기와 영문 키 이름을 해석한다.
- 메뉴에 없는 Ctrl+A/C/X/V/Z는 같은 소유 창의 표준 Edit/RichEdit 컨트롤에서 처리한다.
- 비활성/중복/owner-draw 항목, 잘못된 modifier 조합, 다른 창의 focus, 취소·실행 게이트·시간 제한은 거부한다.
- 메뉴 탐색은 깊이 8/512개로 제한한다. disabled owner를 제외하고 같은 Job의 활성 팝업도 캡처해 Save As를 관찰한다.
- 사용자 정의 UI, GPU 창, 메뉴에 표시되지 않는 임의 단축키 및 모든 앱의 동작을 보장하지 않는다.

`verification/app_shortcuts_test.cpp`는 실제 Guest에서 Ctrl+S 명령 ID/notification, 정확한 Shift 조합,
disabled/중복 메뉴, 실행 게이트, modal owner, Ctrl+A/C/X/V/Z 내용, 외부/custom focus 거부를 검증했다.
공식 빌드 스크립트로 Runner와 회귀 실행 파일을 만들고 테스트 실행 파일은 inbox_import로 들여왔다.
Router RUN-0001은 GUEST_PROCESS_VERIFIED/exit 0으로 완료됐고 stdout은 검사 후 반출됐다.

사용자 승인으로 이전 테스트 VM을 종료한 후 새 Runner로 같은 최신 main 기반 관리 서비스에 연결했다.
실제 SCRP `computer_hotkey` Ctrl+A → GUI 한글·영문 입력 → Ctrl+S에서 ACT-000015 SUCCESS,
메모장 제목의 미저장 표시 제거, 새 Output 후보 ART-000004, Defender EXPORTED를 확인했다.
반출된 `ctrl-s-verified.txt`의 UTF-8 내용은 화면의 세 문단과 정확히 일치한다(160바이트).
기존 WM_CHAR 경로는 CR/LF를 각각 처리하므로 이 시험의 CRLF 입력은 문단 사이 빈 줄로 저장됐다.
Ctrl+Shift+S로 실제 Save As 창이 열리고 취소 후 원문으로 돌아왔다. 그 과정에서 Guest의
`C:\Windows\System32\config\systemprofile\Desktop` 부재 경고가 나와 확인 버튼으로 닫았다.
이 별도 Guest 프로필 문제는 이번 단축키 수정에서 해결하지 않았으며 Save As 파일 쓰기 성공으로 주장하지 않는다.

배포 Runner: `C:\SCRPAppLab\shortcut-fix-20261111\bin\sandbox_runner.exe`
(SHA-256 `0ac93c861f90444af65e8c51dc63ff35f3d542a686a163531cb30a28cc99c5ba`).
기본 solution.json의 runner_exe/runner_sha256과 실제 검증 세션 바이너리가 일치하며 프로필 검증도 통과했다.
세션 `SES-6f082df6fea145b7bf22f985ec9c5fa0`은 종료하지 않고 유지했다.
증거: `C:\SCRPAppLab\shortcut-fix-20261111\verification-summary.json`, `run/019-0.png`(저장),
`run/027-0.png`(Save As), `run/034-0.png`(원문 복귀).
