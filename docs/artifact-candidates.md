# Artifact 후보 감지 — 설계 변경

Runner의 책임은 `Output 변경 감지 → 안정화 확인 → Artifact 후보 보고`다.
Windows Sandbox 내부 Defender가 실행되지 않는 환경에서도 이 경로는 동작한다.
악성코드 검사 및 SHA-256/MIME/크기 검증은 Host Quarantine의 책임이다.
후보 보고는 파일의 안전성, 최종 쓰기 완료 또는 반출 승인을 뜻하지 않는다.

## 기존 구현에서 유지·변경한 부분

| 기존 구성 | 처리 |
| --- | --- |
| `output_watcher.h/.cpp` | 수정 없이 유지. 재귀 감시, 경로 경계, 오류 및 중지 처리 재사용 |
| `PrescanQueue`의 큐·debounce·worker·generation | `CandidateDetector`로 이름과 결과 상태를 변경하고 구조 재사용 |
| `scan_file`의 경로 체인 검사·메타데이터 비교 | `file_stability`로 분리하여 재사용 |
| Defender 실행·프로세스/파이프·결과 파서 | 제거 |
| 검사용 임시 복사본·SHA-256 계산·검사용 크기 제한 | 제거. Runner는 파일 내용을 읽거나 복사하지 않음 |
| Defender 전용 테스트·문서 | 후보 감지 테스트·이 문서로 대체 |

이 변경은 이전 이슈 #3의 Defender 사전 검사 요구를 대체하는 최신 설계에 따른다.
Host 검사나 통신 구현을 Runner로 옮기지 않는다.

## 안정화와 후보 이벤트

`CandidateDetector::submit(OutputChange)`는 감시 콜백에서 큐에만 넣는다.
기존 worker가 로그·파일 관찰·상태 콜백을 처리한다.

1. 생성·수정·새 이름 알림을 파일별로 묶고 마지막 변경 이후 debounce를 기다린다.
2. 기존 경로 체인 검사로 Output 내부 일반 파일을 열어 식별자, 크기, 생성·수정 시각을 관찰한다.
3. 관찰 시 잠시 쓰기·삭제 공유를 거부한다. 쓰기 핸들이 열려 있으면 재시도한다.
   핸들은 관찰 직후 닫으며 대기 중에는 파일을 잠그지 않는다.
4. debounce 간격을 두고 두 관찰이 일치하면 해당 generation의 후보를 한 번 보고한다.
   관찰이 다르면 다시 기다린다. 큐에 들어온 변경을 먼저 적용하므로 이전 generation은 보고하지 않는다.

기본 debounce는 300ms, 안정화 대기 제한은 30초다. `CandidateOptions`의 내부 설정이며
프로토콜 확정값이 아니다. 시간 제한까지 쓰기 잠금이나 불안정 상태가 계속되면 `UNAVAILABLE`을 보고한다.
새 변경 알림을 받으면 다시 시도한다. 대기 중인 한 파일이 다른 파일의 관찰을 막지 않는다.
큐와 기록 파일 수의 상한은 각각 4096이며 초과 시 감시 상태 저하를 보고하고 후보를 무효화한다.

크기는 두 관찰의 일치 여부에만 사용한다. 콘텐츠 해시·MIME·Host 수신 크기 검증은 수행하지 않는다.
메타데이터가 같아도 관찰 직후 다시 쓸 수 있고, 악의적으로 메타데이터를 유지한 변경도 있을 수 있다.
따라서 안정화는 관찰 기반 판단이며 변경 불가능한 파일의 보장이 아니다.

내부 `StatusSink(const CandidateStatus&)` 콜백은 다음 상태를 받는다.

| 상태 | 의미 |
| --- | --- |
| `pending` | 최근 변경의 debounce 대기 |
| `stabilizing` | 추가 관찰 또는 쓰기 핸들 해제 대기 |
| `candidate` | 이 generation의 `ARTIFACT_CANDIDATE` 이벤트 |
| `unavailable` | 삭제·이름 변경·경로 거부·시간 초과·감시 누락·종료 등으로 사용 불가 |

콜백 데이터는 Output 상대 경로, generation, 상태, Win32 오류, 설명이다.
후속 연결은 `candidate`만 보지 말고 이후 변경·무효화도 반영해야 한다.
콜백은 worker에서 호출되므로 빠르게 반환해야 하고 안에서 `stop()`을 호출하면 안 된다.
`get_status()`는 새 알림이 처리 대기 중이거나 감시가 중지·저하된 경우 이전 후보를 유효하게 반환하지 않는다.
실제 파일 접근·전송 시에는 경로와 현재 세대·파일 상태를 다시 검증해야 한다.

현재 Runner는 내부 이벤트를 다음과 같이 콘솔에 출력한다.

```text
ARTIFACT_CANDIDATE "reports\\result.txt" generation=4 error=0 stable observations; candidate only, Host Quarantine verification required
```

이 로그는 SCRP 메시지가 아니다. 향후 확정된 계약에 연결할 때 기존 설계에 따라
`SECURITY_EVENT`의 category `ARTIFACT_CANDIDATE`에 매핑한다. 새로운 최상위 wire 타입이나
JSON 필드, 업로드 기능은 이번 변경에서 정의하지 않는다.

## 경계와 종료

- Output 밖 경로, 경로 순회, reparse point, 디렉터리, hardlink, ADS는 기존 정책대로 후보에서 제외한다.
- 삭제·이전 이름은 기존 후보를 무효화한다. 상위 폴더 삭제·이름 변경은 하위 기록도 무효화한다.
- 일반 폴더 수정 알림만으로는 자식 파일 후보를 무효화하지 않는다.
- 기존 감시 범위를 유지한다. 시작 전 파일이나 통째로 이동된 폴더의 기존 자식들을 열거하지 않는다.
  이동된 자식은 이후 개별 변경 알림이 와야 후보가 된다. 전체 트리 주기 검사도 없다.
- 감시 누락·오류·큐 초과는 전체 후보를 무효화하며 재시작이 필요하다.
- 종료 시 worker 대기를 깨우고 join한다. Defender 자식 프로세스와 검사용 임시 복사본은 생성하지 않는다.

## 재현 및 검증

MSYS2 UCRT64에서:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRUNNER_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure -V
```

`output_watcher`는 기존 감시 검증과 실제 Runner의 후보 로그·Ctrl+C 종료를 확인한다.
`artifact_candidate`는 debounce, 같은 세대의 중복 억제, 안정화 중 변경·삭제·이름 변경,
메타데이터 재관찰, 쓰기 잠금 재시도·시간 초과, 경로 경계, 실제 재귀 감시→후보 연결,
범위 밖 제외, 감시 누락·큐 초과·종료·콜백 예외를 검증한다.

Sandbox에는 빌드된 `sandbox_runner.exe`, `candidate_tests.exe`, `output_watcher_tests.exe`와
`tests/sandbox_candidate_check.ps1`를 같은 읽기 전용 전달 폴더에 둔다.
Sandbox PowerShell에서 아래를 실행한다. 테스트 도구는 제품 배포 대상이 아니다.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\RunnerCandidateTest\sandbox_candidate_check.ps1
$LASTEXITCODE # 0: 모든 검사와 테스트용 Runner 종료 확인
```

로그는 Sandbox 바탕 화면의 `runner-candidate-check-*` 폴더에 남긴다.
테스트 파일은 임시 Output에서 만들고 정리하며 실제 작업 Output은 수정하지 않는다.

2026-09-27 검증 결과:

- UCRT64 Release 빌드 및 CTest 2개 통과.
- PowerShell에서 Windows EXE 실행 및 도움말 종료 코드 0.
- 실행 중인 Windows Sandbox의 로그인 사용자 계정에서 두 테스트 EXE 모두 종료 코드 0.
- 실제 Runner의 `ARTIFACT_CANDIDATE` 로그와 Ctrl+C 종료 확인. 새로 남은 테스트용 Runner 프로세스 없음.
- Host의 symlink 생성 테스트는 권한 부족(1314)으로 건너뛰었으며 junction 경계는 검증함.
- 제품 EXE는 Windows 기본 DLL에만 의존하며 Defender/CNG 라이브러리 의존성을 제거함.
- 새로 생성한 Sandbox에서도 로그인 준비 후 `sandbox_candidate_check.ps1`를 실행해 종료 코드 0을 확인함. 두 테스트 EXE의 성공, 실제 Runner 후보 로그·Ctrl+C 종료, 새 잔존 Runner 없음이 함께 검증됨. 해당 실행 로그는 Sandbox 바탕 화면의 `runner-candidate-check-20260927-032451` 폴더에 있음(해당 Sandbox 종료 시 삭제됨).

Host Quarantine의 검사와 전송·SCRP 연동은 이 저장소의 현재 테스트 범위가 아니다.
