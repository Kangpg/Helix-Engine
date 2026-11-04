# Phase 0 — 기반: 추상화 경계 + C ABI 설계

> 기록일: 2026-06-01 · 상태: 부분 완료

## 목표
모든 확장축(플랫폼·전송·언어·노드)이 의존할 **추상화 경계와 C ABI 계약을 코드로 확정**한다. 이 경계를 초기에 제대로 잡아야 이후 단계에서 코어를 다시 뜯지 않는다. (계획서 Phase 0)

## 완료한 작업
모두 **신규 추가만** 했으며, 기존 `HelixCore`/`GameServer` 파일은 수정·삭제하지 않았다.

공개 SDK — `include/helix/` (구매자가 보는 경계, Win32 타입 비노출)

- `types.h` — `SessionId`, `ByteSpan`, `TransportKind`(Tcp/Kcp/Quic), `CloseReason`, `Status`, `Endpoint`, `ToString(CloseReason)`
- `session.h` — `class Session`: `Id / Send / Close / Remote / Transport / Set·GetUserData`
- `server_app.h` — `class IServerApp`: `OnStart / OnConnect / OnMessage(필수) / OnDisconnect / OnStop`
- `server.h` — `class Server`: `FromConfig(path) / Run(app) / Stop / FindSession / SessionCount` (pimpl)
- `helix.h` — umbrella 헤더
- `helix_c.h` — **안정적 C ABI**: 불투명 핸들 `helix_server`, `helix_status`/`helix_close_reason`/`helix_bytes`, `helix_callbacks`, `helix_server_create/set_callbacks/run/stop/destroy`, `helix_session_send/close/remote`, `helix_abi_version`(=1)

내부 추상화 — `src/`

- `net/INetBackend.h` — 플랫폼 I/O 백엔드. proactor 통일: `Init/Register/PostAccept/PostRecv/PostSend/Poll/Wake`. `NativeHandle`, `IoContext`, `IoCompletion`
- `transport/ITransport.h` — 전송 계층. `ITransport`(`Init/Listen/Send/Close/Kind`) + `ITransportSink`(`OnAccepted/OnData/OnClosed`) + `CreateTransport(kind)` 팩토리
- `cluster/IBackplane.h` — 백플레인. `IBackplane`(세션 디렉터리 `Register/Unregister/Locate/Heartbeat`, 라우팅 `RouteToSession`, 데이터 버전 `PublishDataVersion`, 공유 KV `KvSet/KvGet`) + `IBackplaneSink`
- `data/RuntimeData.h` — 런타임 데이터 핫스왑. `Snapshot<T>`(`Load/Store/Exchange`, `std::atomic<std::shared_ptr<const T>>` 기반), `Versioned<T>`

부수 산출물

- `docs/phase-logger.agent.md` — 단계 기록용 서브에이전트 정의(`.claude/agents/phase-logger.md`로 이동해 사용)

## 설계 결정
- **다언어는 C ABI 단일 경계로 분기.** 예외 비전파(에러코드), 불투명 핸들, 콜백 스레딩, ABI 버전 규칙을 `helix_c.h` 주석에 명문화. C++/C# SDK가 이 위를 얇게 감싼다(msquic·GNS 방식).
- **백엔드는 proactor 스타일로 통일.** Windows IOCP/RIO는 네이티브, Linux epoll은 내부에서 proactor 에뮬레이션 — 상위 코어 무변경 목표.
- **QUIC 비대칭 흡수.** msquic이 자체 I/O를 가지므로 `ITransport::Init`의 `backend` 인자를 옵셔널(nullptr 허용)로 둠. TCP/KCP는 backend 사용, QUIC은 무시.
- **데이터 핫스왑 ↔ scale-out 연결.** `IBackplane::PublishDataVersion` → 각 노드 `Snapshot::Store`로 전 클러스터 동시 갱신. 불변 스냅샷 + atomic 교체로 torn read 없음, refcount 자동 회수.
- **공개 헤더에 저수준 타입 비노출.** `include/helix/`(공개) vs `src/`(내부) 물리 분리.

## 미완료 / 다음 단계로 이월
- CMake + vcpkg 크로스플랫폼 빌드, `CMakePresets.json`, README 정합
- 기존 `HelixCore`/`GameServer`를 새 `include/`·`src/` 구조로 마이그레이션
- Logger 재작성(stdout 구조화·비동기·레벨·스레드 안전)
- 결함 정리: `Network.h` 전역 인스턴스 단일화, `abstract`→`= 0`, `RPMALLOC_ENABLE_OVERRIDE` 매크로 제거, 설정 파일명 정합
- 기존 `Iocp`/`SocketExtension`/`RioBufferPool`을 `INetBackend` Windows 구현으로 이식
- (검토 중) 구현체를 설정에 따라 DLL 플러그인으로 로드하는 모듈 경계 — 채택 시 C ABI에 모듈 등록 진입점 추가 필요

## 검증 상태
**미검증(컴파일 안 함).** 이 세션의 Linux 샌드박스가 비활성(가상화 비활성)이고 프로젝트는 Windows/MSVC/vcpkg 빌드라 헤더를 빌드에 연결해 확인하지 못했다. 현재 산출물은 컴파일 의존이 없는 인터페이스 정의이며, Visual Studio에서 빌드에 연결해 검증해야 한다.

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md
- 에이전트 정의: ../phase-logger.agent.md
