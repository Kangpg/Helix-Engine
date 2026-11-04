# Phase 1 — TCP MVP (Windows IOCP): 실제로 통신되는 최초 버전

> 기록일: 2026-06-01 · 상태: 부분 완료 (코드 완성, 빌드·실행 미검증)

## 목표
지금까지 인터페이스만 있던 네트워크 계층을 **실제 동작하는 에코 서버**로 만든다. Phase 0의 결손 1~3번(Accept 없음 / Session 없음 / recv·send 없음)을 메운다. 범위는 Windows + 일반 IOCP + TCP. (계획서 Phase 1)

## 완료한 작업 (1차 증분 — I/O 백엔드 토대)
모두 신규 추가. 기존 `HelixCore`/`GameServer`는 수정하지 않음.

- `src/net/INetBackend.h` — **수정**: `bool useRio` 제거 → `IoBackendKind{ Auto, Iocp, IocpRio, Epoll }` enum + `ToString` 도입. `IoContext`에 `acceptSocket` 추가 및 `CreateContext/DestroyContext` 컨텍스트 수명 메서드 추가, `Wake(count)`.
- `src/net/WindowsIocpBackend.h` / `.cpp` — **신규**: `INetBackend`의 Windows 구현.
  - `Init`: IOCP 핸들 생성, `WSAIoctl`로 `AcceptEx` 포인터 로드. RIO 요청은 탐지 후 Phase 4까지 일반 IOCP로 폴백(`Name()`이 실제 모드 보고).
  - `Register`(IOCP associate), `CreateContext/DestroyContext`(OVERLAPPED 품은 `WinIoContext`), `PostAccept`(AcceptEx), `PostRecv`(WSARecv), `PostSend`(WSASend).
  - `Poll`: `GetQueuedCompletionStatusEx`로 완료 배치 회수(최대 128), `OVERLAPPED→WinIoContext` 복원(`offsetof`).
  - `Wake`: 전용 완료 키(`kWakeKey`)로 `PostQueuedCompletionStatus` — graceful shutdown 신호. `Poll`에서 `ctx==nullptr`로 전달.
- `src/net/NetBackendFactory.h` / `.cpp` — **신규**: `CreateNetBackend(cfg)` — 플랫폼/설정별 백엔드 생성 + 능력탐지/폴백 캡슐화. (`#if _WIN32` 분기, Linux는 Phase 4 TODO)
- `HELIX_ENGINE_PLAN.md` 2.2절 — 백엔드 선택(`auto/iocp/iocp_rio/epoll`) 표 + 폴백 규칙 + 트레이드오프 반영.

## 설계 결정
- **백엔드 선택은 설정 주도.** 일반 IOCP만 강제하지 않고 `IoBackendKind`로 사용자가 선택. `iocp_rio` 미지원 환경은 경고 후 `iocp` 폴백, `Name()`이 실제 활성 모드 보고.
- **RIO 데이터 경로는 Phase 4로 분리.** Phase 1은 IOCP로 통신부터 뚫는 데 집중. RIO 선택은 받아들이되 동작은 폴백.
- **IoContext 수명은 백엔드가 소유.** 플랫폼 세부(OVERLAPPED)를 파생 타입(`WinIoContext`)에 숨기고, 상위는 `CreateContext/DestroyContext`만 사용 → epoll 백엔드도 동일 계약.
- **MVP 단순화:** `Poll`의 오류 매핑은 정상 완료=비움, recv 0바이트=상위에서 peer-closed 처리. 정식 NTSTATUS→error_code 매핑은 Phase 4 보강.

## 완료한 작업 (2차 증분 — 에코 경로)
- `src/session/Session.h` / `.cpp` — **신규**: TCP 연결 1개. **단일 참조카운트** 수명관리(`AddRef/Release`, ref=1 매니저 + 진행 IO마다 +1, 0 도달 시 자기 삭제 + 맵 제거). `Start`(첫 recv), `OnRecvComplete`(에코→재무장), `Send`(큐잉+직렬화), `OnSendComplete`, `Close`(멱등, closesocket→진행 IO 취소). 송신은 `StartNextSendLocked`로 락 안에서 게시하되 **Release는 항상 락 밖에서**(0 도달 delete 레이스 차단). overlapped WSASend는 전량 전송 완료 가정.
- `src/session/SessionManager.h` / `.cpp` — **신규**: `Create`(한계 검사 + ID 발급 + 맵 등록), `Find`, `Remove`(맵에서만 제거, 삭제 X), `Count`, `CloseAll`(ref 스냅샷 후 락 밖에서 Close — 반복 중 변형 방지). MVP는 `unordered_map`+mutex(사전할당 풀은 Phase 4).
- `src/net/Acceptor.h` / `.cpp` — **신규**: listen/bind/`listen`, `AcceptEx` N개 동시 게시, 완료 시 `SO_UPDATE_ACCEPT_CONTEXT`→`Register`→`Create`→`Start`→같은 컨텍스트 재무장. IPv4(MVP).
- `src/core/ServerCore.h` / `.cpp` — **신규**: 백엔드(팩토리)+SessionManager+Acceptor+I/O 워커 스레드를 묶는 러너. `WorkerLoop`이 `Poll`→`op`별 디스패치(Accept/Recv/Send, `ctx==nullptr`은 Wake). `Start/Run/Stop` — **Stop은 신호만, join·정리는 Run이 전담**(이중 join 레이스 수정).
- `samples/echo_phase1/main.cpp` — **신규**: `ServerCore` 직접 사용하는 임시 에코 하니스(WSAStartup + Ctrl-C로 Stop). Phase 2에서 공개 SDK로 교체.

## 설계 결정 (2차)
- **단일 refcount 수명관리** 채택 — 라이브 서버의 shared_ptr+Link 이중 카운트 문제 회피. close↔진행 IO 완료 레이스는 "모든 ref 빠질 때 삭제"로 차단.
- **Release는 락 보유 중 호출 금지** 규칙 — 0 도달 시 `delete this`가 락 파괴로 이어지는 UB 방지. 송신 경로 전반에 적용.
- **overlapped WSASend 전량완료 가정**으로 부분전송 분기 제거(단순·안전). 비정상 시 bytes==0 → Close.
- **Stop/Run 책임 분리** — Stop은 시그널 안전(신호만), Run이 join+정리 단독 수행.

## 빌드 통합 (완료)
- `CMakeLists.txt` — **신규**: `helix` 정적 라이브러리(엔진 코어) + `helix_echo_phase1` 샘플. Windows 게이트(비-Windows는 `return()`로 스킵, Linux는 Phase 4). `ws2_32`/`mswsock` 링크, C++20, include 경로 `include/`+`src/`.
- `CMakePresets.json` — **신규**: `windows-x64`(의존성 없음) / `windows-x64-vcpkg`(후속 deps용) + debug/release 빌드 프리셋.
- `docs/BUILD.md` — **신규**: 빌드·실행·봇 검증 절차 + Phase 1 종료 체크리스트.

## 미완료 / 다음 단계로 이월
- graceful drain: 종료 시 잔여 완료를 끝까지 배수(현재는 미배수 세션 잔존 가능) — Phase 4
- 사전할당 세션 풀, NTSTATUS→error_code 정식 매핑 — Phase 4
- Winsock 초기화 위치 정리(현재 샘플이 WSAStartup) → Phase 2/3 부트스트랩으로 이동
- 원격주소(`getpeername`) → `Session::Remote`, idle 타임아웃 — Phase 2/4

## 검증 상태
**미검증(컴파일 안 함).** 이 세션의 Linux 샌드박스가 비활성이고 본 코드는 Windows/MSVC 전용(Win32 IOCP, `ws2_32`/`mswsock` 링크)이라 빌드·실행 확인 불가. Visual Studio(x64)에서 새 파일들을 프로젝트에 추가해 빌드 후, `samples/echo_phase1`를 실행하고 검증한다.

검증 도구: `tools/echo_bot.py` (순수 Python, 빌드 불필요). 단계별로 재사용한다.
- 에코 정확성 + 동시접속: `python tools/echo_bot.py --port 5000 --clients 200 --messages 50 --size 256`
- 세션 수명/refcount(close 레이스) 스트레스: `python tools/echo_bot.py churn --port 5000 --clients 500 --rounds 20`
- 통과 시 종료코드 0(불일치/타임아웃/연결오류=1). 특히 `AcceptEx`/`OVERLAPPED` 복원, refcount 수명은 churn 모드로 실측 필수.

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md
- 이전 단계: ./PHASE0.md
