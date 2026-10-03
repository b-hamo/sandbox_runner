# sandbox_runner

Windows Sandbox 내부에서 GUI 관찰·입력, 세션 Output 감시, Artifact 후보 보고 및 Host가 승인한 HTTPS 파일 전송을 수행하는 단일 C++ Runner다. Host의 MCP/Broker·승인·수신 검사·최종 반출과 연결하며 Runner는 파일 안전성을 판정하지 않는다.

## 실행 모드

```powershell
.\sandbox_runner.exe --host-bootstrap C:\RunnerPackage\bootstrap.json --host-address 192.168.0.3
.\sandbox_runner.exe --control-context C:\RunnerPackage\control-context.json
.\sandbox_runner.exe --session-context C:\RunnerPackage\telemetry-context.json
```

- `--host-bootstrap`: 기존 GUI·PNG HTTPS 업로드·세션 Output 감시. 신뢰된 bootstrap에 `control_contract: "artifact-export-v1"`와 `artifact_upload: {"port": <Host HTTPS 포트>, "path": "/scrp/v1/artifacts/"}`를 함께 지정하면 파일 반출 경로를 활성화한다. Host도 새 프로파일을 구현해야 한다.
- `--control-context`: 기존 Control 관리 모드. GUI/Artifact 전송은 활성화하지 않는다.
- `--session-context`: 기존 독립 Telemetry 모드. `event_contract: "artifact-candidate-v1"` 선택 시 네 필드 후보 SECURITY_EVENT를 전송한다. 개발·테스트용 `--output`은 이 모드에서 기존 절대 디렉터리에만 사용할 수 있다.

Host bootstrap 모드의 Output은 `C:\RunnerWorkspace\Sessions\session-<session_id>\runtime-<runtime_id>\generation-<generation>\output`이다. 독립 모드 기본값은 `C:\RunnerWorkspace\Output`이다. 모두 Guest 내부 폴더이며 Host에 쓰기 가능한 공유 Output을 만들지 않는다. 경로·기존 파일을 요청에서 임의로 재지정하지 않는다.

## Artifact 반출

파일 생성 → 안정화 → `SECURITY_EVENT(ARTIFACT_CANDIDATE)` → Host 후보 등록/승인 → `ARTIFACT_REQUEST` → 현재 후보·핸들 재검증 → HTTPS PUT → `ARTIFACT_RESULT` 순서다. 후보에는 event_id·관찰 시각·상대 경로만 보고하며 파일 바이트·해시는 넣지 않는다.

Control과 Telemetry는 별도 WSS다. 검증한 HELLO_ACK의 별도 자격으로 Telemetry를 시작하고 CHANNEL_ACK 후 후보 생산을 연결한다. GUI 준비용 Output 알림 감시는 이전처럼 HELLO 전에 시작한다. 기존 파일이나 후보 보고 준비 전 변경을 초기 스캔으로 보충하지 않으므로 Host는 채널과 기존 Startup Verification이 준비된 후 작업을 시작해야 한다.

파일·경로 체인 핸들을 보유하고 reparse/ADS/하드링크·변경·삭제·크기 상한을 재검증한다. 단일 worker가 고정 HTTPS 경로로 64 KiB 버퍼를 사용해 스트리밍하며 Control heartbeat를 막지 않는다. 별도 토큰·기한·일회성 ID·TLS 검증을 적용하고 빈 201만 수신 성공으로 처리한다. 종료·채널 장애 시 전송을 취소하고 worker/핸들을 정리한다.

Host가 비공개 수신 파일의 크기·SHA-256을 계산하고 HTTP 완료와 Runner 결과를 교차 확인해야 한다. 형식·백신 검사와 승인 후 동일 바이트만 최종 결과 폴더에 반출한다. Runner 업로드 성공은 검사나 EXPORTED 상태가 아니다.

[Host 구현 계약·Schema·체크리스트](docs/artifact-export-contract.md), [Runner 구현·검증·제한](docs/artifact-export-implementation.md), [현재 구조](docs/project-structure.md)를 따른다.

## Host 빌드와 배포

Windows x64 **MSYS2 UCRT64** GCC/CMake/Ninja와 JsonCpp를 같은 환경에서 사용한다. 현재 GUI 기반의 C++17 요구 및 JsonCpp 정적 연결을 유지한다. 도구 버전은 팀에서 새로 동결하지 않는다.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/sandbox_runner.exe --help
```

빌드는 Host에서 수행한다. Sandbox에는 EXE 및 필요한 실행 결과만 배치하며 CMake/GCC/Ninja 설치를 전제로 하지 않는다. Windows TLS chain·hostname과 bootstrap 인증서 pin을 모두 확인한다. 신뢰 인증서 배치는 외부 Runtime 책임이며 평문 fallback이나 인증서 검증 해제는 없다.

제품은 Windows 기본 DLL만 사용하도록 정적 연결한다. `build/`와 테스트 소스·스크립트·테스트용 CMake가 있는 `tests/`는 Git 제외이며 새 checkout 제품 빌드는 이 파일들을 참조하지 않는다.

## 관련 문서

- [Output 안정화·후보 감지](docs/artifact-candidates.md)
- [기존 후보 payload·EVENT_ACK](docs/artifact-candidate-contract.md)
- [Telemetry API·독립 Context](docs/telemetry.md)
- [기존 GUI 통합](docs/gui-integration.md)
- [기존 Host 관찰 연결·Sandbox 검증 이력](docs/host-observation-integration.md)

실제 Host/MCP/Windows Sandbox 전체 Artifact 반출은 Host의 새 계약 구현 후 검증한다. 구현된 기능·테스트 peer 결과와 후속 통합 검증을 구분한다.
