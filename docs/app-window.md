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
프로세스 시작 ACK는 완료 증거가 아니며 앱 화면과 검사된 Router 영수증으로 확인한다.
진단 stdout/stderr는 Guest C:\RunnerLogs에만 쓰며 명시된 로그/NUL 핸들만 자식에게 상속한다.
