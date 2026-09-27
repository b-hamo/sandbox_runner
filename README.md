# sandbox_runner

Windows Sandbox 내부의 세션 Output 변경을 감시하는 단일 C++ Runner다.
Sandbox 하나를 세션 하나로 사용한다. `--session-context <파일>`로 외부 Context를 주입하고
Telemetry READY 이후 `C:\RunnerWorkspace\Output`을 준비한다. 하위 폴더까지 감시하며
정상 종료 요청 또는 감시 오류가 발생할 때까지 실행된다. Context 없이 실행하면 명시적으로 실패한다.

## 현재 범위

- `src/main.cpp`: 공통 실행 진입점, Context·`--output` 입력, 로그 및 Ctrl+C/Ctrl+Break 종료.
- `src/session_context.*`, `src/runner_lifecycle.*`: 기존 Context 재사용·외부 입력 검증·Telemetry READY 확인·종료 연결.
- `src/runner_paths.h`: 세션의 고정 Output 경로. C++ 구성 요소는 이 상수를 공유한다.
- `src/artifact/output_watcher.{h,cpp}`: Windows `ReadDirectoryChangesW` 기반 감시.
- `src/artifact/candidate_detector.{h,cpp}`: 기존 이벤트 큐를 재사용한 파일별 debounce·안정화·세대·후보 상태 관리.
- `src/artifact/file_stability.{h,cpp}`: 기존 경로 검증과 파일 메타데이터 비교를 분리한 안정화 관찰.
- `src/protocol/scrp/`, `src/transport/telemetry/`: Artifact와 독립적인 SCRP Telemetry WSS·ACK·재전송 계층. [API·검증](docs/telemetry.md).
- `CMakeLists.txt`: 제품 Runner만 빌드한다.

### 팀원이 코드를 연결할 위치

| 구분 | 위치 | 역할·배포 여부 |
| --- | --- | --- |
| Runner 진입점 | `src/main.cpp` | Control과 Artifact를 연결할 공통 진입점 |
| 세션 경로 | `src/runner_paths.h` | Sandbox 내부 고정 Output 경로의 기준 |
| Artifact 구현 | `src/artifact/` | Runner에 포함되는 감시 코드 |
| 로컬 검증 코드 | `tests/` | Git 제외. 저장소 빌드·제품 배포에 포함하지 않음 |
| 빌드 산출물 | `build/` | EXE와 CMake 캐시. Git 제외 |

제품 실행 파일은 `sandbox_runner.exe` 하나다. 테스트 소스·스크립트와 테스트용 CMake는
로컬 `tests/`에만 보관하고 Git에서 제외한다. 새 checkout은 테스트 파일 없이 제품을 빌드한다.

향후 Control과 Artifact의 구현 소스를 같은 실행 타깃에 연결한다.
[이슈 #1](https://github.com/b-hamo/sandbox_runner/issues/1)의 감시 기능에
[이슈 #3](https://github.com/b-hamo/sandbox_runner/issues/3)의 최신 설계 변경에 따라 파일 안정화와 Artifact 후보 보고를 연결한다.
Runner는 Defender 검사와 SHA-256/MIME/크기 검증을 수행하지 않는다. 이 검증은 Host Quarantine의 책임이다.
GUI 제어, Artifact의 SCRP 연동, 업로드 및 Host의 최종 반출 승인은 포함하지 않는다.
Telemetry는 외부 Context로 시작하며 Control HELLO_ACK 입력은 아직 미구현이다.
입력 파일과 테스트/Host 계약의 경계는 [Session Context 입력 절차](docs/telemetry.md)를 따른다.
안정화 판단·후보 이벤트·제한·검증 결과는 [후보 감지 문서](docs/artifact-candidates.md)를 따른다.
C++ 표준 버전은 팀 합의 전까지 CMake에서 고정하지 않으며 사용 중인 컴파일러 기본값을 따른다.

## 감시 동작과 경계

고정 경로 구조는 다음과 같다. 세션 ID별 하위 디렉터리는 만들지 않는다.

```text
C:\RunnerWorkspace\         Sandbox 내부 세션 작업 공간
└─ Output\                  파일을 만드는 작업이 결과물을 저장할 위치
   └─ reports\result.txt    하위 폴더도 감시 대상
```

`src/runner_paths.h`의 `runner::default_output_path`가 기준이다.
기본 실행은 누락된 폴더만 생성하고 기존 파일을 지우지 않는다.
경로 준비 시에도 기존 디렉터리와 상위 경로의 reparse point를 거부한다.
현재 프로그램의 Output 허용 정책은 이 고정 경로이며, 별도 세션 승인 프로토콜을 새로 만들지 않는다.
Bootstrap과 파일을 생성하는 구성 요소도 같은 경로를 사용해야 한다.
Host에 쓰기 가능한 공유 폴더로 매핑하지 않는다.

`--output`은 Host 개발·테스트용 경로 재정의이며 **이미 존재하는** 절대 경로를 받는다.
운영 Bootstrap은 이 옵션 없이 Runner를 실행한다. 이 옵션에 외부 작업 요청의 임의 경로를 연결하지 않는다.

- Output 전체 하위 트리의 생성·수정·삭제·이름 변경 전/후 이름을 보고한다. 실행 중 새로 생긴 하위 폴더도 포함한다.
- 이벤트 이름은 Output 기준 상대 경로다. 예: `reports\result.txt`. 하위 폴더 이름이 바뀐 뒤의 변경도 새 경로로 보고한다.
- 알림은 중복되거나 합쳐질 수 있다. 이름 변경의 이전/새 이름을 별도로 전달하며 무조건적인 쌍을 보장하지 않는다.
- 경로 밖 파일은 감시하지 않는다. 상대 경로, `..`, UNC/장치 경로, 드라이브 루트 및 Output/상위 경로의 reparse point는 거부한다.
- 검증한 Output과 상위 폴더 핸들을 삭제 공유 없이 유지한다. 감시 중에는 이 폴더들의 이름 변경·삭제가 제한된다.
- 자식 링크의 이름 자체는 비신뢰 알림으로 보고할 수 있지만 링크를 따라가거나 파일을 열지 않는다.
- 시작 전부터 존재하거나 실행 중 생성된 junction의 외부 대상 파일은 감시하지 않는다. 기존 파일 목록을 시작 시 나열하는 기능은 포함하지 않는다.
- 오류 또는 알림 버퍼 초과 시 Win32 오류를 보고하고 비정상 종료한다. 누락을 숨기거나 자동으로 완전 복구했다고 판단하지 않는다.
- Ctrl+C/Ctrl+Break는 대기 중 I/O를 취소하고 완료를 회수한 뒤 핸들을 정리한다. 강제 프로세스 종료나 콘솔 창 닫기는 정상 종료 경로로 보장하지 않는다.

`artifact::watch_output(path, stop_event, on_change)`는 호출 스레드에서 대기하며 변경 콜백을 호출한다.
`ready`는 첫 감시 요청을 등록한 뒤 전달한다. 콜백은 짧게 처리하고, 후속 모듈의 큐에 연결할 수 있다.
호출자는 수동 리셋 종료 이벤트를 소유하며 함수가 반환할 때까지 유지한다.
Control 내부 구현에 의존하지 않으며 자체 작업 스레드를 만들지 않는다.
Runner는 이 감시 콜백에서 `CandidateDetector::submit()`만 호출한다. 별도 작업 스레드가
파일 안정화를 관찰하고 `ARTIFACT_CANDIDATE` 이벤트와 변경·무효화 상태를 출력한다.
반환값은 정상 중지 시 `ERROR_SUCCESS`, 실패 시 Win32 오류 코드다.
감시 실패 시 후보 큐와 이전 결과도 무효화한다. 정상 종료 시 작업 스레드의 대기를 깨우고 합류한다.
파일 접근·전송을 추가할 때에는 세션과 경로를 다시 검증해야 한다. 알림은 파일 완성이나 안전 판정이 아니다.

## Host에서 빌드 및 실행

MSYS2 **UCRT64** 터미널에서 저장소 루트로 이동한다.
UCRT64용 GCC, CMake, Ninja가 설치되어 있어야 한다.
Telemetry의 JSON 처리를 위해 UCRT64 JsonCpp도 필요하다 (`pacman -S mingw-w64-ucrt-x86_64-jsoncpp`).
JsonCpp는 정적으로 연결하므로 Sandbox에 별도 JsonCpp DLL을 설치하지 않는다.
`command -v cmake ninja g++` 결과가 모두 `/ucrt64/bin/` 아래인지 확인한다.
다른 컴파일러로 만든 `build/` 캐시가 있다면 해당 폴더를 먼저 별도 위치에 백업한다.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/sandbox_runner.exe --help
echo $?
```

도움말에 기본 경로와 재귀 감시 동작이 표시되며 종료 코드는 `0`이다.
실제 실행에는 유효한 Context와 연결 가능한 TLS/WSS peer가 필요하다. Host 검증에는 아래 `--output` 예제를 사용한다.
MSYS2 터미널 밖의 PowerShell에서도 저장소 루트에서 확인한다.

```powershell
& .\build\sandbox_runner.exe --help
$LASTEXITCODE
```

실제 감시는 PowerShell 창 두 개에서 재현한다. 첫 창에서:

```powershell
$outputPath = Join-Path $env:TEMP 'runner-session-example\Output'
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
& .\build\sandbox_runner.exe --session-context C:\session\context.json --output $outputPath
```

`WATCHING` 출력 후 두 번째 창에서:

```powershell
$outputPath = Join-Path $env:TEMP 'runner-session-example\Output'
Set-Content -LiteralPath (Join-Path $outputPath 'test.txt') -Value 'first'
Add-Content -LiteralPath (Join-Path $outputPath 'test.txt') -Value 'second'
Rename-Item -LiteralPath (Join-Path $outputPath 'test.txt') -NewName 'renamed.txt'
New-Item -ItemType Directory -Path (Join-Path $outputPath 'reports\daily') -Force | Out-Null
Set-Content -LiteralPath (Join-Path $outputPath 'reports\daily\result.txt') -Value 'nested'
Add-Content -LiteralPath (Join-Path $outputPath 'reports\daily\result.txt') -Value 'changed'
Rename-Item -LiteralPath (Join-Path $outputPath 'reports\daily\result.txt') -NewName 'final.txt'
Set-Content -LiteralPath (Join-Path (Split-Path $outputPath) 'outside.txt') -Value 'outside'
```

첫 창에서 `CREATED`, `MODIFIED`, `RENAMED_OLD`, `RENAMED_NEW`를 확인한다.
`reports\daily\result.txt` 및 `reports\daily\final.txt`의 이벤트도 확인한다.
`outside.txt`는 보고되지 않아야 한다. Ctrl+C 후 `Watch stopped`와 `$LASTEXITCODE`의 `0`을 확인한다.
로그 이름은 UTF-8이며 제어 문자는 이스케이프한다. 로그는 SCRP 메시지가 아니다.

필요한 DLL은 UCRT64의 `objdump -p build/sandbox_runner.exe`에서 `DLL Name` 항목을 확인한다.
현재 MinGW 빌드에서는 GCC/C++/스레드 런타임을 정적으로 연결해 Host의 DLL 검색 경로에 의존하지 않도록 한다.
Windows 기본 DLL 외의 런타임 DLL이 필요하면 사용한 UCRT64 도구 체인의 DLL을 EXE와 함께 배치한다.

## 검증 정책

자동 테스트 코드는 로컬에만 보관한다. 공유 CMake에는 테스트 타깃·옵션이 없으며,
기존 `RUNNER_BUILD_TESTS`, `RUNNER_TEST_LOCAL_TLS` 옵션은 제품 빌드에서 사용하지 않는다.
이전에 수행한 검증 결과는 아래에 이력으로 남긴다. 새 checkout에서 수동 확인은 위 실행 절차를 따른다.

Junction 거부는 PowerShell에서 별도로 재현할 수 있다:

```powershell
$checkRoot = Join-Path $env:TEMP ('runner-junction-' + [guid]::NewGuid().ToString('N'))
$target = Join-Path $checkRoot 'target'
$alias = Join-Path $checkRoot 'alias'
New-Item -ItemType Directory -Path (Join-Path $target 'nested') -Force | Out-Null
New-Item -ItemType Junction -Path $alias -Target $target | Out-Null
& .\build\sandbox_runner.exe --session-context C:\session\context.json --output $alias
$LASTEXITCODE # 1, Win32 error 5
& .\build\sandbox_runner.exe --session-context C:\session\context.json --output (Join-Path $alias 'nested')
$LASTEXITCODE # 1, Win32 error 5
```

## 실제 Windows Sandbox 확인

Host에서 빌드한 EXE와 필요한 런타임 DLL만 Sandbox 내부의 테스트 폴더로 복사해 실행한다.
Sandbox에 CMake, Ninja 또는 GCC를 설치하지 않는다.
Sandbox 내부에서 신뢰 앵커와 유효한 Context를 준비하고 `--session-context <파일>`로 실행한다.
`--output`을 생략하여 Telemetry READY 이후 고정 경로 준비와 상주를 확인한다.
위 PowerShell 파일 변경 절차의 `$outputPath`를 `C:\RunnerWorkspace\Output`으로 바꾸어
파일 변경과 Ctrl+C 종료를 확인한다.
Host 폴더를 매핑하여 전달한다면 읽기 전용으로 제한하며, Host에 쓰기 가능한 Output 공유 폴더를 만들지 않는다.

Sandbox 자동 시작은 Runtime 담당의 `.wsb` `LogonCommand`/Bootstrap에서 Context를 전달하도록 연결한다.
이 저장소는 Sandbox를 생성·종료하는 Runtime 구현을 추가하지 않는다.
Runner는 감시 대기 중에도 계속 실행되며, 아직 Control 실행 루프와는 연결되지 않았다.
관리되는 종료는 Runner 중지 요청 → 감시 I/O 정리 완료 → Sandbox 종료 순서로 연결해야 한다.
Sandbox 강제 종료 시의 정상 정리 완료는 보장하지 않는다.

현재 개발 환경에서는 UCRT64 Release 빌드와 Host Windows 통합 테스트를 통과했다.
MSYS2/MinGW를 PATH에서 뺀 PowerShell에서도 EXE 및 통합 테스트를 실행했다.
Junction 자체와 상위 Junction 경로 거부를 확인했다. 심볼릭 링크 테스트는 권한 부족(1314)으로 건너뛰었다.
기존 Artifact 구현은 실제 Windows Sandbox에서 후보 감지·감시 테스트와 Runner 후보 로그·Ctrl+C 종료를 확인했다.
Issue #7의 Telemetry lifecycle 통합은 Host Windows에서 검증했으며 실제 Sandbox·팀 Host 연동은 미검증이다.
OS 감시 버퍼 강제 초과 및 디스크 오류 재현은 아직 검증하지 않았다. 내부 큐 초과는 테스트했다.

## 후속 합의

- C++ 표준, GCC 및 외부 라이브러리 버전.
- Runtime Bootstrap 및 파일 생성 구성 요소에서 확정된 고정 경로를 사용하는 연결.
- Control과 Artifact의 시작·종료를 단일 진입점에서 연결하는 방식.
- 알림 누락 시 재동기화 정책.
