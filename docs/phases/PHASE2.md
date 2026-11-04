# Phase 2 — 메시징(프레이밍) + 공개 SDK 연결 + C ABI

> 기록일: 2026-06-01 · 상태: 완료 (코드 완성, 빌드·실행 미검증)

## C ABI 증분 (완료)
- `src/capi/helix_c.cpp` — **신규**: `helix_c.h`를 공개 `Server`/`IServerApp`/`Session` 위에 구현. `CallbackApp`(helix_callbacks→IServerApp 어댑터, session_id 전달), `helix_server_create/set_callbacks/run/stop/destroy`, `helix_session_send/close/remote`, `helix_server_session_count`, `helix_abi_version`. 예외는 try/catch로 차단(경계 비전파), `Status→helix_status` 매핑. → C# 등 바인딩 토대.
- `CMakeLists.txt`에 `src/capi/helix_c.cpp` 추가.
- 알려진 한계: `helix_session_send`가 임의 sid로 호출될 때 FindSession→Send 사이 세션 수명 레이스 가능(콜백 내 사용은 안전). 정식 refcount 핸들 반환은 Phase 4.

## 목표
Phase 1의 raw 에코를 **프레임 단위 메시징 + 공개 SDK**로 끌어올린다. 구매 업체가 저수준 없이 `IServerApp` 핸들러 + `Server`만으로 서버를 만들 수 있게 한다. (계획서 Phase 2)

## 완료한 작업
- `src/proto/FrameCodec.h` — **신규(header-only)**: 길이 프리픽스 프레이밍. `FrameCodec`(headerSize 2/4, bigEndian, **maxMessageSize 가드**), `EncodeHeader`/`DecodeHeader`, `FrameReader::Feed`(부분 헤더/페이로드 누적 → 완결 프레임 콜백, 과대 길이 시 프로토콜 오류). 라이브 서버의 "길이+데이터만(손상 감지 없음)" 약점을 maxMessageSize로 보완.
- `src/session/Session.{h,cpp}` — **수정**: 이제 공개 `helix::Session` 구현(`Id/Send(ByteSpan)/Close(reason)/Remote/Transport/Set·GetUserData`). 수신은 `FrameReader`로 잘라 `IServerApp::OnMessage`, 송신 `Send`는 헤더를 붙여 큐잉(세션별 직렬화 + **송신 큐 한계 백프레셔**→`BackpressureFull`). `Start()`에서 `OnConnect`, `Close(reason)`에서 `OnDisconnect`(1회). Phase 1의 단일 refcount 수명관리/락 밖 Release 규칙 유지.
- `src/session/SessionManager.{h,cpp}` — **수정**: `SessionFactoryConfig`(recvBufSize/maxSendQueue/FrameCodec) + `IServerApp*` 주입. `Create(sock, remote)`로 원격 Endpoint 전달.
- `src/net/Acceptor.cpp` — **수정**: 수락 후 `getpeername`→`Endpoint` 구성해 `Create(sock, ep)`.
- `src/core/ServerCore.{h,cpp}` — **수정**: `IServerApp*`/`FrameCodec`를 설정으로 받아 세션에 주입. `Start(app)`, `Find(id)`(→`helix::Session*`). **Winsock 수명(WSAStartup/Cleanup)**을 여기서 관리.
- `src/core/Server.cpp` — **신규**: 공개 `helix::Server` 구현(pimpl→ServerCore). `FromConfig(path)`(Phase 2는 기본값, 경로 보관), `Run(app)`(OnStart→블로킹→OnStop), `Stop`, `FindSession`, `SessionCount`.
- `samples/echo_phase1/main.cpp` — **교체**: 공개 SDK만 사용하는 `EchoApp : IServerApp` + `Server::FromConfig(...).Run(...)`. Win32/IOCP/프레이밍 비노출.
- `tools/echo_bot.py` — **수정**: 길이 프리픽스 프레이밍 기본(4B big-endian, `--header-size`/`--little` 조정), `--raw`로 Phase 1 호환. echo/churn 모두 프레임 대조.
- `CMakeLists.txt` — **수정**: `src/core/Server.cpp` 추가.

## 설계 결정
- **공개 경계에 저수준 비노출** — `helix::Session`(불투명)·`IServerApp`·`Server`만 노출. 내부 `core::Session`이 공개 인터페이스를 구현.
- **프레이밍은 엔진 책임** — 앱은 메시지 단위만 다룬다. 송신 시 자동 헤더 부착, 수신 시 자동 재조립.
- **maxMessageSize 가드** — 과대/손상 길이를 프로토콜 오류로 즉시 종료(라이브 서버 약점 보완).
- **백프레셔** — 송신 큐 한계 초과 시 `BackpressureFull` 반환(무한 적체 방지).
- **수명 이벤트 1회 보장** — OnConnect(Start)/OnDisconnect(Close exchange 승자)로 정확히 1회.

## 미완료 / 다음 단계로 이월
- **C ABI 구현**(`src/capi/helix_c.cpp`) — `helix_c.h`를 Server/IServerApp 위에 구현(C# 바인딩 토대). **다음 증분.**
- per-session 콜백 직렬화 — OnDisconnect가 in-flight OnMessage와 겹칠 수 있음(무상태 앱은 무해). Phase 4.
- 설정: `FromConfig`가 현재 기본값만. 실제 TOML + 환경변수 로딩은 Phase 3.
- 공개 라이브러리 include를 `include/`만으로 축소(현재 샘플이 `src/` 일부 참조 안 함 — 공개 샘플은 `helix/`만 사용하므로 Phase 3에서 `src/` PUBLIC 제거 가능).

## 검증 상태
**미검증(컴파일 안 함).** Windows/MSVC 전용. VS(x64) 빌드 후 `helix_echo_phase1` 실행 → 봇으로 검증:
- 프레임 에코 정확성/동시접속: `python tools/echo_bot.py --port 5000 --clients 200 --messages 50 --size 256`
- 세션 수명/refcount: `python tools/echo_bot.py churn --port 5000 --clients 500 --rounds 20`
- 프레이밍 경계: `--size 0`(랜덤 1~1024)로 부분 수신/재조립 확인. 과대 길이 거부는 별도 악성 클라이언트로 확인(후속).

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md
- 이전 단계: ./PHASE1.md
