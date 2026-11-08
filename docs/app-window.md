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

지원 범위: Win32/WinForms 창의 왼쪽 클릭, 이동, Unicode 텍스트, 일부 제어 키와 Edit의 Ctrl+A,
전용 Win+R 실행 창. 전체 바탕화면, OS Run 창, 모든 단축키·앱·GPU 창·IME 조합을 지원한다고 주장하지 않는다.
Job에 kill-on-close를 설정하지 않으므로 Runner 연결 종료만으로 앱을 죽이지 않는다.
Runner 재시작 후 기존 앱 Job의 소유권 복구는 미구현이다.
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
