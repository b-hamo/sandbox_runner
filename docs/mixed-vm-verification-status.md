# micro_VM 연동 검증 상태 — 2026-11-07

이 문서는 초기 점검 이력이다. 이후 앱 창 연동과 2026-11-08 실제 WSB A + micro_VM B/C 실행은
host_control PR #115의 `docs/mixed-vm-followup-20261108.md`에 기록했다.
2026-11-08 후속 시험에서 실제 중계 재연결·한글 유지·검사 반출을 확인했다.
최초 전체 바탕화면 목표는 미완료다. [앱 창 계약](app-window.md)을 함께 참조한다.

관련: b-hamo/sandbox_runner#37, b-hamo/host_control#113, b-hamo/micro_VM#1.

- 작업 기준: 원격 develop `cee101389f438be006f093e8fdb40d79e8cc2195`.
- 기존 사용자 작업 폴더의 미커밋 변경을 보존하고 별도 작업 트리를 만들었다.
- 이번 기준 소스의 Runner 빌드와 micro_VM 내부 실행은 **미검증**이다.
- 환경 점검에 사용된 기존 SCRP WSB Runner의 SHA-256은
  `2a9c0f156e74aa0371a34ba6f66515359846e4fab101a9168ee2e2e4e2205641`이다.
  이 바이너리는 이번 develop에서 새로 빌드한 결과가 아니며, 신규 Runner 검증 증거로 사용하지 않는다.
- 새 micro_VM B 시작이 WSB 점유 검사에서 거부돼 B/C Runner의 인증·캡처·입력·Win+R·반출은 시험하지 못했다.

소스 조사에서는 Host의 `sandbox_run.py`에 WDAGUtilityAccount의 Desktop/Input 경로가 고정돼 있고,
`sandbox_run_guest.ps1`에도 계정명 검사가 남아 있다. 단순히 검사를 삭제하지 않고,
Manager가 정한 VM/세션 소유권과 Guest 실행 경로를 인증된 SCRP 계약에 연결해야 한다.
기존 bootstrap pin, TLS 검증, 정책 버전, heartbeat, 세대·action 중복 방지, Output 전송 검사는 유지한다.

실제 검증 재개 시 UCRT64로 위 기준과 변경 소스를 VM 안에서 빌드하고, 실행한 EXE의 해시를 기록한다.
Host/Manager의 실제 로드 경로와 소스 버전을 함께 고정한 뒤, 각 VM에서 Runner를 시작한다.
부팅 또는 프로세스 생성은 READY가 아니며 실제 인증·관찰·heartbeat·반출 완료까지 확인한다.
이번 문서 추가는 Runner 기능 구현 완료를 뜻하지 않는다.
