# helix-engine — 코드 지도 (CODE_MAP)
> 코드와 불일치 시 코드가 정본. 갱신일: 2026-11-04

## 개요 (레이어 한눈에)

Helix.Engine은 연결·세션·프레이밍·송수신을 담당하는 범용 네트워크 서버 엔진이다. 앱이 메시지 형식과 처리 로직을 정의한다. Helix.Mmo와 이 저장소의 Helix.Protocol은 콘텐츠·메시지 조립 예제로, 엔진 코어와 분리되어 있다. 병렬 월드도 선택 콘텐츠 실험에 해당한다.

관리 프로젝트 공통 target은 `csharp/Directory.Build.props`의 net10.0이고 루트 global.json이 stable .NET 10 SDK를 선택한다. 기존 net8 이력과 현재 실행을 혼동하지 않는다. 관리 NuGet은 0.4.0이며 이전 0.3.0과 .NET 10 전환 검증은 Phase 이력으로 보존한다.

현재 C++와 C#은 독립 엔진이다. C++ 공개 SDK는 ServerCore로 연결되며 선택 C ABI가 SDK를 감싼다. 순수 C# HelixServer는 IServerTransport에 의존하며 기본 TcpServerTransport가 async sockets/Channels를 조립한다. 두 언어의 Helix.Mmo는 네트워크에 의존하지 않고 인스턴스별 이동·전투·AOI 구현을 주입받는다. 아래 native 상세는 C++ 경로에 적용하며 C# P/Invoke 설명은 현재 경로가 아니다.

C++ 코어는 INetBackend/ITransport/IBackplane에 의존하고 각 구현을 팩토리로 받는다. TCP는 backend+Acceptor+Session, KCP는 자체 UDP I/O+TransportSession 경로다. Windows는 일반 IOCP 및 실제 IOCP+RIO를 선택한다. QUIC은 미구현이며 C#과의 네이티브 호출 관계는 없다.

---

## 공통 메시지 및 독립 Helix.Protocol

- `protos/helix_mmo.proto`: 버전/요청 ID, 이동/공격/조회 oneof, 접속/큐 응답/월드 상태의 wire 계약.
- `cpp/Helix.Protocol/CMakeLists.txt`: protoc로 build 디렉터리에 C++ 메시지 생성, helix::protocol 공개 target 및 install. `cmake/HelixProtobuf.cmake`는 고정 Protobuf 36.2와 동일 protoc, Abseil 의존성을 복원한다. HELIX_BUILD_PROTOCOL=OFF면 코어/오프라인 MMO에 의존성을 만들지 않는다.
- `cpp/Helix.Protocol/include/helix/protocol/codec.h`, `src/Codec.cpp`: TryDecodeClient는 파싱, IsValid는 버전/명령/좌표 검증, TryEncode는 응답 직렬화, MakeReply는 큐 응답 생성.
- `csharp/Helix.Protocol/Helix.Protocol.csproj`: 빌드 전용 Grpc.Tools 2.84.0 + 관리 Google.Protobuf 3.36.2. 생성 코드는 obj에 있고 gRPC 서버를 만들지 않는다.
- `csharp/Helix.Protocol/PacketCodec.cs`: 동일 검증 계약과 응답 생성. Engine/Mmo에 의존하지 않는다.
- 두 WorldServer 샘플이 바이트 ↔ 생성 메시지 ↔ 도메인 명령을 변환하고 세션 ID를 주입한다. 전송/월드 라이브러리는 Protobuf를 알지 않는다.
- `tools/generate_protocol.py`, `requirements-protobuf.txt`: 같은 스키마의 Python 테스트 클라이언트 생성/runtime. `world_bot.py`는 두 서버 이동·전투·AOI와 invalid/unknown-field/request-id 호환성을 실제 연결로 검증한다.

---

## include/helix/ (공개 SDK 상세)

### helix.h — C++ 사용자가 단 하나만 include 하는 umbrella 헤더 (공개)
- (클래스/구조체 없음) — `types/session/server_app/server/runtime_data` 헤더를 한데 모아 노출한다.

### helix_c.h — 엔진 코어를 C ABI로 노출하는 단일 안정 경계 (공개, C ABI 계약)
- 계약/역할: 예외는 경계를 넘지 않고 모든 실패는 `helix_status`로 반환, 객체는 불투명 핸들로만 다루며, 콜백은 워커 스레드에서 호출될 수 있고 `helix_bytes.data`는 콜백 동안만 유효, 구조체 필드는 끝에만 추가하고 `helix_abi_version()`으로 호환 확인.
- `helix_status` (enum) — C 측 결과 코드(OK/CONFIG/INVALID_SESSION/BACKPRESSURE_FULL/NOT_FOUND 등).
- `helix_close_reason` (enum) — C 측 세션 종료 사유(`SessionCloseReason` 대응).
- `helix_bytes` (struct) — 소유권 없는 바이트 뷰(`data`+`len`, 콜백 동안만 유효).
- `helix_server` (불투명 핸들) — 서버 인스턴스를 가리키는 전방 선언 포인터 타입.
- `helix_callbacks` (struct) — 앱 콜백 묶음(on_connect/on_message(필수)/on_disconnect/on_data_version + ctx).
  - `helix_abi_version()` — ABI 버전 정수를 돌려줘 런타임 호환성을 확인한다. (공개)
  - `helix_server_create(config_path, out)` — TOML+환경변수로 서버를 만들어 불투명 핸들을 채운다. (공개)
  - `helix_server_set_callbacks(s, cb)` — 앱 콜백 묶음을 서버에 등록한다. (공개)
  - `helix_server_run(s)` — Stop/시그널까지 블로킹 실행한다. (공개)
  - `helix_server_stop(s)` — 다른 스레드에서 graceful shutdown을 트리거한다. (공개)
  - `helix_server_destroy(s)` — 서버 핸들을 파괴한다. (공개)
  - `helix_session_send(s, sid, msg)` — 세션에 메시지를 전송한다(프레이밍 자동). (공개)
  - `helix_session_close(s, sid, reason)` — 세션을 사유와 함께 닫는다. (공개)
  - `helix_session_remote(s, sid, ip_buf, len, port)` — 세션의 원격 IP/포트를 조회한다. (공개)
  - `helix_server_session_count(s)` — 활성 세션 수를 돌려준다. (공개)
  - `helix_session_route(s, target, msg)` — SessionId로 노드 투명 전송한다(로컬 직접/타 노드 백플레인). (공개)
  - `helix_cluster_publish_data_version(s, channel, version)` — 데이터 버전을 전 노드에 브로드캐스트한다. (공개)
  - `helix_cluster_kv_set(s, key, value, ttl_sec)` — 클러스터 공유 KV에 값을 쓴다. (공개)
  - `helix_cluster_kv_get(s, key, buf, len, out_len)` — 공유 KV에서 값을 읽는다(잘림은 out_len으로 감지). (공개)

### runtime_data.h — 런타임 데이터 핫스왑(불변 스냅샷 + atomic 포인터 교체) (공개)
- `Snapshot<T>` (클래스) — 불변 스냅샷을 atomic shared_ptr로 무중단 교체(읽기 락 없음).
  - `Load()` — 현재 스냅샷의 소유 복사본을 acquire로 돌려준다(사용 중 회수 안전). (공개)
  - `Store(next)` — 새 스냅샷으로 release 원자 교체한다. (공개)
  - `Exchange(next)` — 새 스냅샷으로 바꾸며 이전 스냅샷을 돌려준다(버전 비교/롤백용). (공개)
- `Versioned<T>` (struct) — 버전 태그(version)를 곁들인 스냅샷(이미 최신이면 스킵 처리용).

### server.h — 서버 부트스트랩 진입점(pimpl로 저수준 타입 은닉) (공개)
- `Server` (클래스) — 설정 로드·실행·세션 조회·scale-out 동작의 공개 파사드(SRP: 부트스트랩/위임).
  - `FromConfig(configPath)` — TOML+환경변수로 서버를 구성한다(검증 실패 시 Run이 ConfigError). (공개, static)
  - `Run(app)` — 앱을 붙여 Stop/시그널까지 블로킹 실행하고 종료코드를 돌려준다. (공개)
  - `Stop()` — 다른 스레드/시그널 핸들러에서 graceful shutdown을 트리거한다. (공개)
  - `FindSession(id)` — SessionId로 활성 세션을 조회한다(없으면 nullptr). (공개)
  - `SessionCount()` — 활성 세션 수를 돌려준다. (공개)
  - `RouteToSession(target, message)` — SessionId로 노드 투명 전송한다. (공개)
  - `PublishDataVersion(channel, version)` — 런타임 데이터 버전을 전 노드에 브로드캐스트한다. (공개)
  - `ClusterKvSet(key, value, ttlSec)` — 클러스터 공유 KV에 값을 쓴다(비활성 시 false). (공개)
  - `ClusterKvGet(key)` — 공유 KV에서 값을 읽는다(비활성/미발견 시 nullopt). (공개)

### server_app.h — 구매 업체가 구현하는 유일한 핵심 인터페이스 (공개, ISP 계약)
- `IServerApp` (인터페이스) — 앱 로직 훅만 노출(OnMessage 필수, 나머지 선택). 저수준은 비노출(ISP).
  - `OnStart(server)` — 서버 기동 직후 1회 호출(선택 훅). (공개, virtual)
  - `OnConnect(session)` — 새 연결 수립 시 호출(선택 훅). (공개, virtual)
  - `OnMessage(session, message)` — 완결 메시지 1건 수신 시 호출(필수 구현). (공개, 순수가상)
  - `OnDisconnect(session, reason)` — 연결 종료 시 호출(선택 훅). (공개, virtual)
  - `OnDataVersion(channel, version)` — 데이터 버전 브로드캐스트 도착 시 호출(선택 훅, scale-out). (공개, virtual)
  - `OnStop()` — graceful shutdown 완료 직전 1회 호출(선택 훅). (공개, virtual)

### session.h — 하나의 클라이언트 연결을 나타내는 앱 측 핸들 (공개, ISP 계약)
- `Session` (인터페이스) — 연결 제어 계약(엔진이 구현, 수명은 엔진 소유: OnConnect~OnDisconnect 유효).
  - `Id()` — 세션 고유 식별자를 돌려준다. (공개, 순수가상)
  - `Send(message)` — 메시지 1건을 전송한다(프레이밍 자동, 세션별 직렬화, 큐 가득 시 BackpressureFull). (공개, 순수가상)
  - `Close(reason)` — 진행 중 송신을 비운 뒤 연결을 닫는다. (공개, 순수가상)
  - `Remote()` — 원격 피어 Endpoint를 돌려준다. (공개, 순수가상)
  - `Transport()` — 이 세션의 전송 종류를 돌려준다. (공개, 순수가상)
  - `SetUserData(ptr)`/`GetUserData()` — 앱 측 per-session 상태 포인터를 보관/조회한다(엔진은 해석 안 함). (공개, 순수가상)

### types.h — 공개 SDK의 핵심 값 타입(플랫폼 타입 비노출) (공개)
- `SessionId` (별칭) — 64비트 세션 식별자(상위 24비트 NodeId, 하위 40비트 노드 로컬 시퀀스).
- `ByteSpan` (별칭) — 앱에 송수신할 불변 바이트 묶음(`std::span<const std::byte>`).
- `TransportType` (enum) — 지원 전송 프로토콜(Tcp/Kcp/Quic).
- `SessionCloseReason` (enum) — 세션 종료 사유(Normal/PeerClosed/IdleTimeout/IoError/ProtocolError/ServerShutdown/Kicked).
- `Status` (enum) — 공개 API 결과 코드(Ok/ConfigError/InvalidSession/BackpressureFull 등).
- `Endpoint` (struct) — 원격 피어 주소의 추상 표현(ip 문자열 + port).
  - `MakeSessionId(nodeId, seq)` — NodeId와 시퀀스를 합쳐 SessionId를 만든다. (공개, constexpr)
  - `NodeOf(id)` — SessionId에서 소유 NodeId를 추출한다. (공개, constexpr)
  - `ToString(reason)` — 종료 사유를 문자열로 변환한다. (공개, constexpr)

---

## src/capi/ (C ABI 구현)

### helix_c.cpp — 공개 C++ SDK 위에 안정 C ABI를 구현(예외 차단) (C ABI 계약 구현)
- `CallbackApp` (클래스, 익명 네임스페이스) — `helix_callbacks`를 `IServerApp`로 변환하는 어댑터(C 콜백 ↔ C++ 훅).
  - `OnConnect/OnMessage/OnDisconnect/OnDataVersion(...)` — 각 SDK 훅을 등록된 C 함수 포인터로 ctx와 함께 위임 호출한다.
- `helix_server` (struct, 핸들 정의) — 공개 `Server` + 저장된 콜백 묶음을 담는 불투명 핸들 실체.
  - `MapStatus(st)` — `helix::Status`를 `helix_status`로 매핑한다.
  - `helix_abi_version()` — `HELIX_ABI_VERSION` 상수를 돌려준다.
  - `helix_server_create(...)` — `Server::FromConfig`를 호출해 핸들을 할당한다(예외→INTERNAL).
  - `helix_server_set_callbacks(...)` — on_message 필수 검사 후 콜백을 저장한다.
  - `helix_server_run(...)` — `CallbackApp`을 만들어 `Server::Run`을 호출한다(예외 차단).
  - `helix_server_stop/destroy(...)` — Stop을 위임하거나 핸들을 delete한다.
  - `helix_session_send/close/remote(...)` — FindSession 후 Send/Close/Remote를 위임하고 상태를 매핑한다.
  - `helix_server_session_count(...)` — SessionCount를 위임한다.
  - `helix_session_route(...)` — `RouteToSession`을 위임한다.
  - `helix_cluster_publish_data_version/kv_set/kv_get(...)` — scale-out KV/버전 동작을 위임하고 결과를 매핑한다(미발견→NOT_FOUND, 잘림→out_len).

---

## src/cluster/ (scale-out 백플레인)

### BackplaneFactory.cpp — 설정에 따라 IBackplane 구현을 생성(DIP/OCP)
- (전역 함수)
  - `ParseBackplaneKind(s)` — "redis" 문자열을 `BackplaneKind`로 파싱한다(그 외 None).
  - `CreateBackplane(kind)` — Redis면 `RedisBackplane`을, None이면 nullptr를 만든다.

### BackplaneFactory.h — 백플레인 종류 enum + 팩토리 선언(DIP/OCP)
- `BackplaneKind` (enum) — 백플레인 종류(None=단일 노드 / Redis=기본 scale-out).
  - `ParseBackplaneKind(s)` / `CreateBackplane(kind)` — 위 구현의 선언.

### IBackplane.h — scale-out 협업 추상화(세션 디렉터리/라우팅/KV/데이터 버전) (추상화 #3 계약)
- 계약/역할: 동질 노드 + L4 LB에서 노드 간 협업을 외부화한다. 엔진은 토폴로지를 강요하지 않고 인터페이스만 제공(Redis 기본, NATS/Kafka 교체 가능).
- `BackplaneConfig` (struct) — 백플레인 연결 설정(url/nodeId/sessionTtlSec).
- `IBackplaneSink` (인터페이스) — 다른 노드→이 노드 방향 수신 콜백 계약.
  - `OnRouted(target, payload)` — 이 노드 보유 세션으로 전달할 메시지를 받는다. (순수가상)
  - `OnDataVersion(channel, version)` — 런타임 데이터 갱신 알림을 받는다. (순수가상)
- `IBackplane` (인터페이스) — 백플레인 동작 계약(디렉터리/라우팅/KV/버전 브로드캐스트).
  - `Connect(cfg, sink)` / `Disconnect()` — 백플레인에 연결/해제한다. (순수가상)
  - `RegisterSession(sid)` / `UnregisterSession(sid)` — 세션 디렉터리에 이 노드 소유로 등록/해제한다. (순수가상)
  - `Locate(sid)` — SessionId가 어느 NodeId에 있는지 조회한다. (순수가상)
  - `Heartbeat()` — 보유 세션 디렉터리 엔트리의 TTL을 갱신한다(좀비 정리). (순수가상)
  - `RouteToSession(target, payload)` — 대상이 다른 노드면 그 노드로 publish한다. (순수가상)
  - `PublishDataVersion(channel, version)` — 데이터 버전을 전 노드에 브로드캐스트한다. (순수가상)
  - `KvSet/KvGet(...)` — 노드 간 공유 KV를 읽고 쓴다. (순수가상)

### RedisBackplane.cpp — IBackplane의 Redis 구현(명령 연결 + 구독 스레드)  (크로스플랫폼)
- (익명 네임스페이스 헬퍼)
  - `ParseUrl(url, host, port, db)` — "redis://host:port/db"를 호스트/포트/DB로 분해한다.
  - `Be64(v)` / `Rd64(p)` — 64비트 정수를 빅엔디안 8바이트로 인코딩/디코딩한다.
- `RedisBackplane` (클래스) — Redis로 디렉터리/라우팅/KV/버전을 구현(명령 연결 + 구독 스레드).
  - `~RedisBackplane()` — Disconnect를 호출해 정리한다.
  - `SessKey(sid)` — 세션 디렉터리 키("helix:sess:{sid}")를 만든다.
  - `Connect(cfg, sink)` — URL 파싱 후 명령 연결을 열고 구독 스레드를 띄운다(실패 시 false).
  - `Disconnect()` — 구독 연결을 닫아 스레드를 깨우고 join 후 명령 연결을 닫는다.
  - `EnsureCmd()` — 명령 연결이 끊겼으면 재연결+SELECT 한다(_cmdMx 보유 가정).
  - `RegisterSession(sid)` — 로컬 집합에 넣고 `SET ... EX ttl`로 디렉터리에 등록한다.
  - `UnregisterSession(sid)` — 로컬 집합에서 빼고 `DEL`로 디렉터리에서 제거한다.
  - `GetNodeLocked(sid)` — `GET`으로 세션 소유 NodeId를 조회한다(_cmdMx 보유 가정).
  - `Locate(sid)` — _cmdMx를 잡고 GetNodeLocked를 호출한다.
  - `Heartbeat()` — 로컬 보유 세션들의 `EXPIRE`를 파이프라인으로 일괄 갱신한다(라운드트립 1회).
  - `RouteToSession(target, payload)` — 대상 노드를 찾아 `helix:route:{node}` 채널로 (sid+payload) publish한다.
  - `PublishDataVersion(channel, version)` — `helix:dataver` 채널로 (version+channel)을 publish한다.
  - `KvSet/KvGet(...)` — `helix:kv:{key}`에 SET(EX 선택)/GET 한다.
  - `OpenSubscriber()` — 구독 연결을 열고 SELECT 후 두 채널을 SUBSCRIBE 한다.
  - `Dispatch(channel, payload)` — 푸시 채널을 분기해 OnRouted/OnDataVersion으로 sink에 올린다.
  - `SubscribeLoop()` — 구독 푸시를 읽어 Dispatch 하고, 끊기면 자동 재연결+재구독한다.

### RedisBackplane.h — RedisBackplane 선언 + 멤버 상태(연결 2개/로컬 세션 집합)  (크로스플랫폼)
- `RedisBackplane` (클래스) — 위 구현의 선언(명령 연결 `_cmd`+mutex, 구독 연결 `_sub`+전용 스레드, 로컬 세션 집합).

### RespClient.cpp — 의존성 0 RESP2 클라이언트 구현(블로킹 소켓)  (크로스플랫폼)
- (익명 네임스페이스)
  - `kInvalid` — INVALID_SOCKET을 `uintptr_t`로 보관한 무효 핸들 상수.
- `RespConn` (클래스) — 단일 Redis 연결의 프레이밍/프로토콜만 담당(SRP).
  - `~RespConn()` / `Close()` — 소켓을 닫고 수신 버퍼/커서를 비운다.
  - `IsOpen()` — 소켓이 유효한지 돌려준다.
  - `Connect(host, port, recvTimeoutMs)` — getaddrinfo→connect로 TCP 연결하고 TCP_NODELAY/수신 타임아웃을 건다.
  - `SendAll(data, len)` — 부분 송신을 반복해 전량 전송한다.
  - `Fill()` — 소켓에서 수신 버퍼로 추가로 채운다(소비분 정리 포함).
  - `ReadLine(line)` — CRLF 종단 한 줄을 읽는다(종단 제외).
  - `ReadBytes(n, out)` — n바이트 + 후행 CRLF를 소비해 읽는다(바이너리 안전).
  - `ParseValue(out)` — RESP 태그(+/-/:/$/*)별로 응답 1건을 재귀 파싱한다.
  - `SendCommand(args)` — 인자 배열을 RESP 요청으로 인코딩해 전송만 한다(파이프라인/구독용).
  - `ReadReply(out)` — 응답 1건을 파싱한다(구독/파이프라인 회수용).
  - `Command(args, out)` — 요청 전송 후 응답 1건을 읽는다(요청/응답 헬퍼).

### RespClient.h — RESP2 응답 값 + 단일 연결 선언(의존성 0)
- `RespValue` (struct) — 파싱된 RESP 응답 1건(Nil/Simple/Error/Integer/Bulk/Array, 바이너리 안전 str).
  - `IsNil()` / `IsError()` — 응답이 Nil/Error 타입인지 돌려준다.
- `RespConn` (클래스) — 위 구현의 선언(블로킹 단일 연결, 한 스레드/외부 mutex 직렬화 전제).

---

## src/config/ (설정)

### EngineConfig.cpp — 파일 파싱 + 환경변수 오버라이드 + 검증 구현
- (익명 네임스페이스 헬퍼)
  - `Trim(s)` / `Lower(s)` / `Upper(s)` — 문자열 양끝 공백 제거/대소문자 변환.
  - `ParseValueToken(v)` — '=' 뒤 값에서 따옴표를 풀고 인라인 주석(#,;)을 제거한다.
  - `ParseUInt(s, def)` / `ParseBool(s, def)` — 문자열을 정수/불리언으로 안전 파싱한다.
- `ConfigLoader` (클래스, 멤버 구현)
  - `Value(section, key, def)` — 환경변수→파일값→기본값 순으로 설정값을 조회한다.
  - `Load(path)` — 파일을 INI/TOML 부분집합으로 파싱→오버라이드→검증한다(실패 시 nullopt+에러 메시지).

### EngineConfig.h — 엔진 설정 스키마 + 로더 선언(12-factor 레이어링)
- `EngineConfig` (struct) — 외부화된 모든 튜닝값(network/buffers/framing/logging/shutdown/cluster 섹션).
- `ConfigLoader` (클래스) — 설정을 로드/오버라이드/검증하는 책임(SRP).
  - `Load(path)` — 위 구현의 선언(없으면 기본값으로 성공). (선언)
  - `GetError()` — 마지막 검증 실패 사유 문자열을 돌려준다.
  - `Value(...)` — 위 조회 헬퍼의 선언. (private)

---

## src/core/ (코어 러너)

### Server.cpp — 공개 helix::Server 구현(pimpl → ServerCore)
- (익명 네임스페이스)
  - `ParseBackend(s)` — "iocp"/"iocp_rio"/"epoll" 문자열을 `IoBackendKind`로 파싱한다.
  - `ParseTransport(s)` — "kcp"/"quic" 문자열을 `TransportType`로 파싱한다(그 외 Tcp).
- `Server::Impl` (struct) — pimpl 실체(설정 경로/ServerCoreConfig/ServerCore/앱/설정오류 플래그).
  - `FromConfig(configPath)` — ConfigLoader로 설정을 읽어 로거를 초기화하고 ServerCoreConfig로 매핑한다.
  - `Run(app)` — 설정 오류면 거부, 아니면 ServerCore를 만들어 Start→OnStart→Run→OnStop 한다.
  - `Stop()` — ServerCore에 Stop을 위임한다.
  - `FindSession/SessionCount/RouteToSession/PublishDataVersion/ClusterKvSet/ClusterKvGet(...)` — ServerCore에 각 동작을 위임한다.

### ServerCore.cpp — 백엔드+Acceptor+SessionManager+워커+백플레인+전송을 묶는 엔진 러너  (크로스플랫폼)
- `ServerCore` (클래스, 멤버 구현)
  - `~ServerCore()` — Stop 후 Run을 호출해 드레이닝/join/정리를 보장한다.
  - `Start(app)` — Winsock 초기화 후 (선택)백플레인 연결, TCP면 백엔드/SessionManager/Acceptor/워커를 띄우고 KCP/QUIC면 ITransport를 Listen 한다.
  - `HeartbeatLoop()` — heartbeatMs마다 백플레인 Heartbeat를 호출하는 전용 스레드 본체.
  - `WorkerLoop(index)` — 백엔드 Poll로 완료를 배치 회수해 Accept/Recv/Send를 해당 핸들러로 디스패치한다(TCP I/O 스레드).
  - `Run()` — Stop 요청까지 블로킹 후 드레이닝(세션 0 대기)→워커 종료→백플레인/백엔드 정리한다(KCP는 전송 Stop 경로).
  - `Stop()` — Acceptor 중단 + 세션 CloseAll + 워커 Wake로 graceful shutdown을 신호만 한다.
  - `Find(id)` — txMode면 `_txSessions`에서, 아니면 SessionManager에서 세션을 조회한다.
  - `SessionCount()` — 활성 세션 수를 모드에 맞게 돌려준다.
  - `BackendName()` — 활성 백엔드 이름을 돌려준다.
  - `RouteToSession(target, message)` — 로컬 우선 직접 송신, 아니면 백플레인으로 전달한다.
  - `PublishDataVersion(channel, version)` — 백플레인이 있으면 브로드캐스트, 없으면 로컬 앱에 직접 통지한다.
  - `KvSet/KvGet(...)` — 백플레인에 공유 KV 동작을 위임한다(없으면 false/nullopt).
  - `OnRouted(target, payload)` — (IBackplaneSink) 보유 세션에 Send 한다(없으면 드롭, at-most-once).
  - `OnDataVersion(channel, version)` — (IBackplaneSink) 앱 OnDataVersion을 호출한다.
  - `OnSessionOpened/OnSessionClosed(id)` — (ISessionObserver) 백플레인 디렉터리에 등록/해제한다.
  - `OnAccepted(conn, remote)` — (ITransportSink) TransportSession을 만들어 등록하고 OnConnect를 호출한다.
  - `OnData(conn, bytes)` — (ITransportSink) shared_ptr로 세션을 잡고 FrameReader로 디프레이밍해 OnMessage 한다.
  - `OnClosed(conn, reason)` — (ITransportSink) 세션을 제거하고 MarkClosed 1회 시 OnDisconnect/디렉터리 해제 한다.

### ServerCore.h — ServerCore 선언 + ServerCoreConfig(세 sink 인터페이스 구현)  (크로스플랫폼)
- `ServerCoreConfig` (struct) — 코어 런타임 설정(bind/backend/transport/maxConnections/스레드/프레이밍/drain/cluster).
- `ServerCore` (클래스) — TCP(INetBackend)·KCP/QUIC(ITransport) 경로를 동일 IServerApp으로 구동하며 `IBackplaneSink`+`ISessionObserver`+`ITransportSink`를 구현(코어는 세 추상화에만 의존).
  - (멤버 선언) — 위 구현의 선언 + `WorkerLoop`/`HeartbeatLoop` private.

### TransportSession.cpp — ITransport 기반 세션의 Send/Close 구현
- `TransportSession` (클래스, 멤버 구현)
  - `TransportSession(...)` — id/transport/conn/codec/remote/kind로 세션을 구성한다.
  - `Send(message)` — 길이 헤더를 붙여 `ITransport::Send`로 전달한다(닫힘/과대 검사).
  - `Close(reason)` — 전송에 연결 종료를 요청한다(OnClosed로 되돌아와 코어가 정리/OnDisconnect).

### TransportSession.h — ITransport 위에서 공개 helix::Session 계약 구현(코어가 shared_ptr 소유)
- `TransportSession` (클래스) — ConnId 기반 전송 위에 공개 Session을 구현(콜백은 직접 부르지 않음, SRP).
  - `Id/Send/Close/Remote/Transport/SetUserData/GetUserData(...)` — 공개 helix::Session 계약 구현.
  - `Conn()` — 전송 수준 ConnId를 돌려준다(ServerCore 전용).
  - `Reader()` — 디프레이밍용 FrameReader 참조를 돌려준다(ServerCore 전용).
  - `MarkClosed()` — 종료를 1회만 확정해 OnDisconnect 중복을 막는다(최초 true).

---

## src/data/ (런타임 데이터)

### RuntimeData.h — 공개 SDK로 승격된 런타임 데이터 유틸의 재노출 셰임
- (타입 없음) — 정식 정의 `include/helix/runtime_data.h`를 include 만 한다.

---

## src/log/ (로깅)

### Log.cpp — 레벨·스레드 안전·stdout/파일 로거 구현
- (익명 네임스페이스) — 전역 mutex/설정/파일 스트림과 `LevelName`/`Timestamp` 헬퍼.
  - `LevelName(l)` — 레벨을 "DEBUG/INFO/WARN/ERROR" 문자열로 변환한다.
  - `Timestamp()` — 현재 시각을 "YYYY-MM-DDThh:mm:ss"로 만든다(플랫폼별 localtime).
- (전역 함수)
  - `ParseLevel(s)` — "debug/warn/error" 문자열을 `Level`로 파싱한다(기본 Info).
  - `Init(cfg)` — 로거 설정을 저장하고 파일 싱크면 파일을 연다.
  - `CurrentLevel()` — 현재 로그 레벨을 돌려준다(매크로의 선필터용).
  - `Write(lvl, msg)` — 타임스탬프+레벨+메시지를 stdout/파일에 mutex 보호로 출력한다.

### Log.h — 로거 인터페이스 + HLOG 매크로(레벨 선필터)
- `Level` (enum) — 로그 레벨(Debug/Info/Warn/Error).
- `LogConfig` (struct) — 로거 설정(level/toStdout/toFile/file).
  - `ParseLevel/Init/Write/CurrentLevel(...)` — 위 구현의 선언.
- `HLOG`/`HLOG_DEBUG`/`HLOG_INFO`/`HLOG_WARN`/`HLOG_ERROR` (매크로) — 레벨을 선검사 후 통과 시에만 Write를 호출한다.

---

## src/net/ (플랫폼 I/O 백엔드)

### Acceptor.cpp — listen 소켓 생성 + AcceptEx/accept 루프 구현 [Windows/Linux]
- `Acceptor` (클래스, 멤버 구현)
  - `Acceptor(backend, mgr)` / `~Acceptor()` — 백엔드/SessionManager 참조를 잡고, 소멸 시 Stop 한다.
  - `CreateAcceptSocket()` — [Windows] 미리 수락 소켓을 만든다 / [Linux] accept4가 수락 시점에 생성하므로 무효 반환.
  - `Listen(ep, backlog, concurrentAccepts)` — listen 소켓을 bind/listen→백엔드 등록 후 동시 accept N개를 사전 게시한다.
  - `PostOneAccept(ctx)` — 수락 소켓을 준비해 백엔드에 AcceptEx를 한 건 건다(재무장 공통 경로).
  - `OnAcceptComplete(ctx, ec)` — [Windows] SO_UPDATE_ACCEPT_CONTEXT 적용 후 등록→피어 주소 조회→세션 생성/Start→재무장 한다.
  - `Stop()` — listen/수락 소켓을 닫고 컨텍스트를 파괴한다.

### Acceptor.h — Acceptor 선언(수락만 담당, SRP) [Windows/Linux]
- `Acceptor` (클래스) — listen+수락 루프만 책임지고 수락 완료 시 SessionManager로 세션 생성을 넘긴다(SRP).
  - (멤버 선언) — 위 구현의 선언.

### EpollBackend.cpp — epoll reactor를 proactor로 에뮬레이션한 INetBackend 구현 [Linux]
- (익명 네임스페이스)
  - `EpollIoContext` (struct) — 부분 송신 진행분(`sentOffset`)을 추가한 IoContext 파생.
  - `SetNonBlocking(fd)` / `Errno(e)` — fd를 논블로킹으로 만들고, errno를 error_code로 변환한다.
- `EpollBackend` (클래스, 멤버 구현)
  - `~EpollBackend()` / `Shutdown()` — eventfd/epoll fd와 등록 맵을 닫고 비운다.
  - `Init(cfg)` — epoll_create1 + eventfd(SEMAPHORE) wake fd를 만들어 등록한다.
  - `Register(handle)` — fd를 논블로킹으로 만들고 FdState를 만들어 EPOLLONESHOT으로 epoll에 추가한다.
  - `CreateContext/DestroyContext(...)` — EpollIoContext를 new/delete 한다.
  - `RearmLocked(st)` — pending 상태로부터 관심 이벤트(EPOLLIN/OUT)를 재계산해 EPOLL_CTL_MOD 한다(_mtx 보유 가정).
  - `ArmRecv/ArmSend(ctx)` — ctx를 pending 슬롯에 넣고 재무장한다(Post/재시도 공통).
  - `PostAccept/PostRecv/PostSend(ctx)` — op를 설정하고 Recv/Send 슬롯에 무장한다(Send는 sentOffset 리셋).
  - `Poll(out, maxOut, timeoutMs)` — epoll_wait로 준비된 fd에서 accept4/recv/send를 실제 수행해 "완료"로 포장한다(부분 송신은 재무장).
  - `Wake(count)` — eventfd에 count를 write 해 대기 중 Poll을 깨운다.

### EpollBackend.h — EpollBackend 선언(reactor→proactor 에뮬레이션) [Linux]
- `EpollBackend` (클래스) — epoll 기반 INetBackend 구현(EPOLLONESHOT으로 한 fd 준비를 한 스레드에만 배달).
- `FdState` (struct, private) — 등록 fd 1개의 비동기 상태(pendingRecv/pendingSend 슬롯).

### INetBackend.h — 플랫폼 I/O 백엔드 추상화(proactor 스타일로 통일) (추상화 #1 계약)
- 계약/역할: Post*()로 비동기 작업을 걸면 Poll()이 "이미 완료된" 작업을 돌려준다(완료 통지). epoll은 내부에서 readiness→실제 I/O→완료로 포장(LSP: 호출자는 구현을 몰라야 동작).
- `NativeHandle` (별칭) — 플랫폼 소켓 핸들의 불투명 표현(Windows SOCKET / POSIX fd).
- `IoBackendKind` (enum) — 설정으로 고르는 백엔드 종류(Auto/Iocp/IocpRio/Epoll).
- `IoOp` (enum) — 비동기 작업 종류(Accept/Recv/Send/Connect).
- `IoContext` (struct) — 한 비동기 작업의 컨텍스트(백엔드가 OVERLAPPED 등 실타입 소유).
- `IoCompletion` (struct) — Poll이 돌려주는 완료 1건(ctx==nullptr이면 Wake 신호).
- `BackendConfig` (struct) — 백엔드 초기화 설정(kind/maxConnections).
- `INetBackend` (인터페이스) — 비동기 I/O 백엔드 계약.
  - `Init(cfg)` / `Shutdown()` — 백엔드를 초기화/정리한다. (순수가상)
  - `Register(handle)` — 소켓을 백엔드에 등록한다(IOCP associate / epoll add). (순수가상)
  - `CreateContext/DestroyContext(...)` — 비동기 작업 컨텍스트 수명을 백엔드가 소유한다. (순수가상)
  - `PostAccept/PostRecv/PostSend(ctx)` — 비동기 작업을 게시한다(완료는 Poll로 회수). (순수가상)
  - `Poll(out, maxOut, timeoutMs)` — 완료를 최대 maxOut개 회수한다(0=타임아웃, 다중 스레드 동시 호출 가능). (순수가상)
  - `Wake(count)` — 대기 중 Poll들을 깨우는 신호를 게시한다. (순수가상)
  - `Name()` — 실제 활성화된 백엔드 이름을 돌려준다(폴백 반영). (순수가상)

### NetBackendFactory.cpp — 플랫폼/설정에 맞는 백엔드를 생성(DIP) [Windows/Linux]
- (전역 함수)
  - `CreateNetBackend(cfg)` — [Windows] WindowsIocpBackend / [Linux] EpollBackend를 만들어 Init 한다(실패 시 nullptr).

### NetBackendFactory.h — 백엔드 팩토리 선언(능력 탐지/폴백 캡슐화, DIP/OCP)
- (전역 함수)
  - `CreateNetBackend(cfg)` — 위 구현의 선언.

### PlatformNet.h — Winsock 스타일 호출을 POSIX에서도 컴파일되게 하는 단일 셰임 [Windows/Linux]
- 역할: 코어/세션/Acceptor가 SOCKET/closesocket/WSA* 표기를 유지하도록 플랫폼 차이를 이 헤더에서만 흡수(공개 헤더에는 비포함). [Linux]에서 SOCKET/WSASocketW/WSAStartup 등을 POSIX 대응으로 정의한다.

### WindowsIocpBackend.cpp — 일반 IOCP 백엔드 구현(AcceptEx+WSARecv+WSASend) [Windows]
- (익명 네임스페이스)
  - `kWakeKey` / `kAddrLen` — Wake 신호용 완료 키와 AcceptEx 주소 버퍼 길이 상수.
  - `WinIoContext` (struct) — OVERLAPPED/WSABUF/accept 주소 버퍼를 품은 IoContext 파생.
  - `FromOverlapped(ov)` — 완료된 OVERLAPPED 포인터에서 WinIoContext를 복원한다.
- `WindowsIocpBackend` (클래스, 멤버 구현)
  - `~WindowsIocpBackend()` / `Shutdown()` — IOCP 핸들을 닫고 상태를 비운다.
  - `Init(cfg)` — IOCP를 만들고 AcceptEx를 로드한다(Name="iocp"). 하이브리드의 완료 알림 source도 주입받는다.
  - `LoadAcceptEx()` — 임시 소켓 + WSAIoctl로 AcceptEx 함수 포인터를 로드한다.
  - `Register(handle)` — 소켓을 IOCP에 연결한다(완료 키 0 고정).
  - `CreateContext/DestroyContext(...)` — WinIoContext를 new/delete 한다.
  - `PostAccept(ctx)` — 미리 만든 수락 소켓으로 AcceptEx를 건다.
  - `PostRecv/PostSend(ctx)` — WSARecv/WSASend로 비동기 수신/송신을 건다.
  - `Poll(out, maxOut, timeoutMs)` — GetQueuedCompletionStatusEx로 완료를 배치 회수한다(Wake 키는 ctx=nullptr).
  - `Wake(count)` — PostQueuedCompletionStatus로 wake 키 완료를 count회 게시한다.

### WindowsIocpBackend.h — 일반 IOCP 및 완료 source 계약 [Windows]
- `WindowsIocpBackend` (클래스) — IOCP 기반 INetBackend 구현. 별도 IIocpCompletionSource를 주입받아 RIO CQ 완료도 dispatch한다.
  - `AcceptExFn()` — 로드된 AcceptEx 함수 포인터를 공개한다(Acceptor용).
  - (그 외 멤버 선언) — 위 구현의 선언.

---

## src/proto/ (프레이밍)

### Windows 하이브리드 및 비교 도구 (추가)

- `src/net/WindowsRioBackend`: INetBackend 구현, IOCP와 RioDataPlane을 조립한다. listen/accept 소켓 모두 RIO flag로 생성한다. 명시적 RIO 초기화 실패는 폴백 없이 보고한다.
- `src/net/RioDataPlane`: RIO 확장 탐지, RQ 레지스트리, CQ/IOCP 알림, 송수신 게시와 완료 회수. RQ/CQ의 동시 접근을 각각 직렬화한다.
- `src/net/RioBuffer`: 컨텍스트별 VirtualAlloc/RIORegisterBuffer, 완료 회수 후 deregistration/free. 고정 등록 버퍼 staging copy를 사용한다.
- `Acceptor.PendingCount`: 미완료 AcceptEx 수. Stop은 취소만 하며 컨텍스트 해제는 완료 배수 후 destructor다. 앱 콜백은 accept 락 밖이다.
- `ServerCore.Run`: native Windows에서 세션/AcceptEx 취소를 모두 회수한 다음 workers/owners를 종료한다. drain budget 초과는 경고하며 Linux는 기존 경로를 유지한다.
- `tests/windows_backend_test.cpp`: 실제 이름을 확인하는 IOCP/RIO 200KB echo와 진행 중 I/O 정상 종료. `tests/session_partial_test.cpp`: 부분 frame/순서, 재게시 실패, 단일 refcount/context 회수.
- `csharp/benchmarks/Helix.SerializationBenchmarks`: Engine/Mmo/Solution과 독립인 여섯 포맷 비교. codec 주입, source-generated JSON/MessagePack/MemoryPack, 공통 domain object, FlatBuffers bulk vector/array.
- `cpp/benchmarks`: HELIX_BUILD_BENCHMARKS=ON 선택 target. 같은 domain 값과 .fbs로 Protobuf/FlatBuffers/Binary를 비교한다.
- `tools/backend_benchmark.py`, `tools/serialization_report.py`: 실제 모드의 같은 Release 부하 비교와 raw 결과 기반 Markdown 보고서. 네트워크/프레임과 직렬화 microbench의 측정 범위를 분리한다.

### FrameCodec.h — 길이 프리픽스 프레이밍(header-only, SRP: 프레이밍만)
- `FrameCodec` (struct) — 프레이밍 설정(headerSize 2/4, 엔디안, maxMessageSize 가드).
  - `EncodeHeader(c, len, out)` — 페이로드 길이를 헤더 바이트로 기록한다(한계 초과 시 false). (inline)
  - `DecodeHeader(c, in)` — 헤더 바이트에서 페이로드 길이를 읽는다. (inline)
- `FrameReader` (클래스) — 수신 스트림에서 완결 프레임만 잘라내는 책임(부분 헤더/페이로드 누적).
  - `Feed(data, len, cb)` — 바이트를 누적하고 완결 프레임마다 cb를 호출한다(과대 길이면 false→세션 종료).

---

## src/session/ (세션 수명)

### ISessionObserver.h — 세션 수명 옵저버(디렉터리 등록/해제 알림, ISP/DIP)
- `ISessionObserver` (인터페이스) — SessionManager 수명 이벤트를 작은 인터페이스로 알린다(scale-out 결합 분리, ServerCore가 구현).
  - `OnSessionOpened(id)` — 세션 생성 직후 호출(디렉터리 등록). (순수가상)
  - `OnSessionClosed(id)` — 세션 최종 해제 시 호출(디렉터리 제거). (순수가상)

### Session.cpp — TCP 세션 구현(수신 디프레이밍/송신 큐잉/단일 refcount 수명) [Windows/Linux]
- `Session` (클래스, 멤버 구현)
  - `Session(...)` / `~Session()` — recv 버퍼 할당 + recv/send 컨텍스트 생성 / 컨텍스트 파괴 + 소켓 close.
  - `AddRef()` / `Release()` — 단일 refcount 증감(0 도달 시 맵에서 Remove 후 delete this).
  - `Start()` — OnConnect를 호출하고 첫 recv를 게시한다.
  - `PostRecv()` — 닫힘이 아니면 recv 버퍼로 비동기 수신을 건다(AddRef/실패 시 Release 보상).
  - `OnRecvComplete(bytes)` — 0바이트면 PeerClosed로 닫고, 아니면 OnData 후 다음 recv를 게시한다(Release).
  - `OnData(data, len)` — FrameReader로 완결 메시지를 잘라 OnMessage로 전달한다(과대 프레임이면 ProtocolError).
  - `Send(message)` — 길이 헤더를 붙여 EnqueueRaw로 큐잉한다(닫힘/과대/한계 검사 → 상태 반환).
  - `EnqueueRaw(framed)` — 송신 큐에 넣고 비-busy면 다음 송신을 시작한다(백프레셔/포스트 실패 처리).
  - `StartNextSendLocked()` — 큐 앞 프레임을 in-flight로 옮겨 PostSend 한다(세션별 송신 직렬화).
  - `OnSendComplete(bytes)` — 완료된 bytes만큼 offset을 진행한다. 나머지를 재게시하며 전체 frame 완료 후 다음 송신을 시작한다. 게시 실패/오류 참조 회수는 락 밖에서 한다.
  - `Close(reason)` — 닫힘을 1회 확정(소켓 close + OnDisconnect 1회 + Release).

### Session.h — TCP 세션 선언(공개 helix::Session 구현체) [Windows/Linux]
- `Session` (클래스) — 소켓+INetBackend에 직접 묶인 공개 Session 구현(수신 디프레이밍/송신 직렬화/단일 refcount).
  - (멤버 선언) — 위 구현의 선언 + 송신 큐/in-flight/락/ref/closed 상태.

### SessionManager.cpp — 세션 생성/조회/제거 구현(단일 refcount 소유)
- `SessionManager` (클래스, 멤버 구현)
  - `SessionManager(...)` / `~SessionManager()` — 백엔드/팩토리/앱/nodeId/옵저버를 잡는다(정상 경로에선 맵이 비어 소멸).
  - `Create(sock, remote)` — 한계 검사 후 SessionId를 발급해 Session을 만들어 맵에 넣고(락 밖에서) 옵저버에 통지한다.
  - `Find(id)` — 맵에서 세션을 조회한다(없으면 nullptr).
  - `Remove(id)` — 맵에서 제거하고(락 밖에서) 옵저버에 닫힘을 통지한다(Release ref==0에서만 호출).
  - `Count()` — 활성 세션 수를 돌려준다.
  - `CloseAll()` — AddRef 스냅샷을 떠 모든 세션을 ServerShutdown으로 닫는다(락 밖에서).

### SessionManager.h — 세션 생성/조회/제거 선언(SRP: 수명/조회만)
- `SessionFactoryConfig` (struct) — 세션 생성 시 주입 설정(recvBufSize/maxSendQueue/frame).
- `SessionManager` (클래스) — 세션 수명/조회만 책임(맵은 raw 포인터, Remove는 삭제 안 함).
  - (멤버 선언) — 위 구현의 선언 + nodeId 임베드/옵저버/시퀀스/맵+락.

---

## src/transport/ (전송 계층)

### ITransport.h — 전송 계층 추상화(TCP/KCP/QUIC 교체) (추상화 #2 계약)
- 계약/역할: 동일 IServerApp이 전송과 무관하게 동작하도록 ConnId 기반 콜백으로 통일한다. TCP/KCP는 backend를 주입받고 QUIC은 무시(비대칭 흡수). 프레이밍은 코어가 담당.
- `ConnId` (별칭) — 전송 수준 연결 식별자(SessionId 매핑 전 단계).
- `ITransportSink` (인터페이스) — 전송→코어 방향 콜백 계약(ServerCore가 구현).
  - `OnAccepted(conn, remote)` — 새 연결 수립을 알린다. (순수가상)
  - `OnData(conn, bytes)` — 프레이밍 이전 raw 바이트를 올린다(누적/프레이밍은 코어). (순수가상)
  - `OnClosed(conn, reason)` — 연결 종료를 알린다. (순수가상)
- `TransportConfig` (struct) — 전송 초기화 설정(bind/backlog/maxConnections).
- `ITransport` (인터페이스) — 전송 동작 계약.
  - `Init(cfg, sink, backend)` — 전송을 초기화한다(TCP/KCP는 backend 사용, QUIC은 nullptr 허용). (순수가상)
  - `Listen()` / `Stop()` — 수신을 시작/중단한다. (순수가상)
  - `Send(conn, bytes)` — 전송 큐로 바이트를 송신한다. (순수가상)
  - `Close(conn, reason)` — 연결을 닫는다. (순수가상)
  - `Kind()` — 전송 종류를 돌려준다. (순수가상)
- (전역 함수)
  - `CreateTransport(kind)` — 설정 종류에 맞는 ITransport 구현을 생성한다.

### TransportFactory.cpp — TransportType에 따라 ITransport 구현 생성(DIP/OCP)
- (전역 함수)
  - `CreateTransport(kind)` — Kcp면 `KcpTransport`를, TCP(기존 경로)/QUIC(미구현)은 nullptr를 만든다.

### kcp/KcpTransport.cpp — ikcp.c 임베드 위 서버측 디먹스 ITransport 구현(저지연 신뢰 UDP)  (크로스플랫폼)
- `KcpConn` (struct) — 전송 연결 1개(ikcpcb + 원격 주소 + output용 소켓 핸들 + lastRecvMs).
- (익명 네임스페이스)
  - `KcpOutput(buf, len, kcp, user)` — ikcp output 콜백: 한 datagram을 원격으로 sendto 한다.
  - `EndpointOf(sa)` — sockaddr_storage를 Endpoint(ip/port)로 변환한다.
  - `NowTickMs()` — steady_clock 기준 밀리초 틱을 돌려준다.
- `KcpTransport` (클래스, 멤버 구현)
  - `KcpTransport()` / `~KcpTransport()` — 소켓을 무효 초기화 / 소멸 시 Stop 한다.
  - `NowMs()` — 시작 기준점 대비 경과 밀리초를 돌려준다(ikcp_update 인자).
  - `Init(cfg, sink, backend)` — UDP 소켓을 bind 하고 수신 버퍼/타임아웃을 건다(backend 무시).
  - `Listen()` — recv/update 스레드를 띄운다.
  - `Stop()` — 소켓을 닫아 스레드를 깨우고 join 후 모든 ikcpcb를 해제한다.
  - `RecvLoop()` — recvfrom으로 datagram을 받아 주소로 연결을 구분(새 주소=새 연결), ikcp_input→ikcp_recv로 조립해 OnAccepted/OnData를 락 밖에서 올린다.
  - `UpdateLoop()` — 10ms마다 전 연결 ikcp_update를 돌리고 idle 타임아웃 연결을 정리해 OnClosed 한다.
  - `Send(conn, bytes)` — ikcp_send 후 ikcp_flush로 즉시 송신한다(ikcp 접근 직렬화).
  - `Close(conn, reason)` — 연결을 제거하고 ikcpcb를 해제한 뒤 OnClosed 한다.

### kcp/KcpTransport.h — KcpTransport 선언(자체 UDP I/O, 프레이밍은 코어 담당)  (크로스플랫폼)
- `KcpConn` (struct, 전방 선언) — ikcpcb + sockaddr 보관(.cpp 정의).
- `KcpTransport` (클래스) — ikcp 임베드 기반 ITransport 구현(전용 UDP 소켓 + recv/update 스레드, INetBackend와 병렬 배치).
  - (멤버 선언) — 위 구현의 선언 + 연결 맵/주소→ConnId 맵/ikcp 직렬화 mutex.

## cpp/ — C++ 독립 프로젝트

### CMakeLists.txt / Helix.Engine/CMakeLists.txt
- C++ 엔진·MMO·샘플·CTest를 구성한다. 루트 CMake는 기존 프리셋 진입점으로 cpp를 호출한다.
- `cmake/HelixIdeFolders.cmake`는 루트·독립 C++ 솔루션에 엔진/모듈/샘플/테스트/벤치마크/외부 의존성/CMake 폴더를 지정한다. 타깃의 FOLDER 표시 속성만 설정하며 imported·alias 타깃은 순회에서 제외한다.
- HELIX_BUILD_CAPI가 ON일 때만 C ABI 공유 라이브러리를 생성한다. public include는 include/이며 src/와 KCP include는 PRIVATE다.

### Helix.Mmo/include/helix/mmo/types.h
- EntityState / Position / MoveRequest / AttackRequest — 월드 값 타입과 입력을 정의한다.
- MovementContext / CombatContext — 읽기 전용 엔티티 뷰와 정책 입력을 전달한다.
- WorldLimits / TickResult / WorldSnapshot — 실행 한도·명령 결과·불변 게시 데이터를 정의한다.

### Helix.Mmo/include/helix/mmo/{movement_policy,combat_rules,visibility_policy}.h
- IMovementPolicy.Resolve(context) — 위치 또는 거부를 계산한다.
- ICombatRules.Resolve(context) — 비음수 피해량 또는 거부를 계산한다.
- IVisibilityPolicy.Select(observer, entities) — 불변 뷰에서 가시 ID를 선택한다(동시 조회 계약).

### Helix.Mmo/include/helix/mmo/delegate_policies.h
- DelegateMovementPolicy / DelegateCombatRules / DelegateVisibilityPolicy — 각 인터페이스에 함수 객체를 연결한다.

### Helix.Mmo/src/WorldInstance.cpp
- WorldInstance — 인스턴스별 엔티티 상태와 주입된 정책을 소유한다.
  - QueueAdd/QueueRemove/QueueMove/QueueAttack(...) — 바운드 큐에 값 명령을 넣는다.
  - ReplaceMovement/ReplaceCombat/ReplaceVisibility(...) — 다음 tick의 pending 구현을 교체한다.
  - Tick() — 단일 writer로 교체/batch를 포착하고 상태를 검증·적용한 뒤 게시한다.
  - Snapshot()/Find(id) — 완료 tick의 불변 데이터를 조회한다(Find는 정렬 벡터 lower_bound).
  - VisibleTo(observer) — 같은 게시 객체의 AOI와 데이터로 조회하고 유효 ID를 정규화한다.

### Helix.Mmo C++ 선택 병렬 월드 (2026-11-03)

- `include/helix/mmo/parallel_world.h`, `src/ParallelWorld.cpp`: ParallelWorldInstance — 바운드 명령/정책을 tick 경계에서 포착하고 lifecycle → 병렬 이동 → 공간 인덱스/피격 계획 → 병렬 판정 → 대상별 병렬 HP 반영 → 원자 게시를 조정한다. legacy WorldInstance의 명령 순차 의미와 구분한다.
- `include/helix/mmo/world_executor.h`, `src/WorldExecutor.cpp`: IWorldExecutor/WorldTask — 선행 작업 ID를 갖는 DAG 실행과 전체 join 계약. 순차 adapter와 Taskflow 4.1.0 adapter는 같은 그래프를 실행한다. 공용 executor를 여러 월드에 주입하며 공개 헤더에 Taskflow 타입을 노출하지 않는다.
- `src/SpatialIndex.h`: SpatialIndex — int32 XZ 셀 색인과 XYZ 거리 후보 질의. 초대형 질의는 전수 검사로 전환한다. 게시 index는 불변이며 VisibleTo의 정책과 동일 snapshot을 사용한다.
- `samples/server.cpp`: WorldApp<TWorld> — 조립 시 기존/병렬 월드를 선택하고 같은 codec/네트워크 경로를 사용한다. 두 월드는 같은 pool을 공유한다. ticker는 여전히 두 월드를 순서대로 호출한다.
- `samples/parallel_benchmark.cpp`, `tools/parallel_world_benchmark.py`: 동일 단계의 inline/Taskflow worker 1/2/4/8 분산·밀집·보스 집중 공격 비교. tick 지연과 별도 AOI 질의·결과 hash를 기록한다.
- `tests/parallel_world_test.cpp`, `tools/verify_parallel_world.py`: 도메인 동시 실행·독립 거리 oracle·설정/실패·실제 C++ Protobuf 서버의 두 실행 모드 검증. C# 빌드 의존 없음.
- `cmake/HelixTaskflow.cmake`, `HELIX_BUILD_PARALLEL_MMO=ON`: 고정 upstream commit 복원. 설치 타깃 `helix::mmo_parallel`, 원 라이선스 포함. 기본 OFF.
- `docs/PARALLEL_MMO.md`: 단계별 게임 의미, 정책 계약, 지원 한계, 조사한 라이브러리와 재현 명령.

### Helix.Mmo/samples/{main,server}.cpp
- main — 델리게이트 교체와 인스턴스 격리를 시연한다.
- WorldApp — 네트워크 콜백을 월드 명령으로 변환하고 별도 tick에서 필드/투기장을 실행한다.

## include/helix/engine_config.h / src/config/

- EngineConfig — 네트워크·프레이밍·클러스터 및 mmo 실행 한도를 외부 설정 값으로 정의한다.
- ConfigLoader.Load(path) — 파일/env를 적용하고 MMO 양수/정수 한도를 검증한다.
- src/config/EngineConfig.h — 공개 설정 선언을 재노출하는 forwarding shim이다.

## csharp/ — 순수 관리 엔진과 독립 솔루션

### Helix.Engine/HelixServer.cs
- HelixServer — IServerTransport를 주입/팩토리로 받아 앱 실행 수명을 관리한다.
  - RunAsync(app, cancellation) — 전송 시작·앱 시작·수락 실행·회수·앱 종료를 조립한다.
  - Stop()/DisposeAsync() — 취소를 요청하고 실행 종료를 기다린다.
  - FindSession()/RouteToSession() — 로컬 활성 세션을 조회/송신한다.

### Helix.Engine/Session.cs / IServerApp.cs
- Session — ISessionTransport 위의 공개 Send/Close 핸들을 제공한다.
- IHelixApp — 세션 connect/message/disconnect 및 앱 시작/종료 콜백 계약을 제공한다.
- DelegateServerApp — 필수 메시지 델리게이트와 선택 연결 훅을 앱 인터페이스로 연결한다.

### Helix.Engine/Transport/
- IServerTransport — 수락/세션 조회/진행 I/O 회수 계약을 정의한다.
- ISessionTransport — 송신/종료의 좁은 연결 계약을 정의한다(Session.cs에 선언).
- TransportFactory.Create(config) — 기본 TCP 구현을 생성한다. 외부 구현은 호스트에 직접 주입한다.
- TcpServerTransport — TCP 수락 루프·세션 등록·최대 연결·종료 회수를 관리한다.
- TcpAcceptor.AcceptAsync(...) — 리스너의 비동기 수락을 수행한다.
- TcpSession.RunAsync(...) — 세션별 콜백 순서와 수신/송신 작업의 수명을 관리한다.
- SendQueue.TryEnqueue()/WriteAsync() — 바운드 Channels로 송신을 직렬화하고 큐 초과를 false로 보고한다.

### Helix.Engine/Protocol/FrameCodec.cs
- Encode(message) — 설정된 길이 헤더를 붙인다.
- ReadAsync(stream, cancellation) — 분할/합친 TCP 바이트를 메시지로 읽고 과대 길이를 거부한다.

### Helix.Engine/Configuration/EngineConfig.cs
- Load(path)/Validate() — 동일 파일/env 규칙과 실행 한도/프레이밍 설정을 검사한다.

### Helix.Mmo/
- Types / IMovementPolicy / ICombatRules / IVisibilityPolicy / DelegatePolicies — C++와 같은 의미의 관리 값/기능 계약을 제공한다.
- WorldInstance.Queue*/Replace*/Tick/Snapshot/Find/VisibleTo — 인스턴스 격리·바운드 입력·tick 교체·불변 조회를 수행한다. 관리 조회/가시성 필터는 선형 탐색이다.

### samples/ / tests/
- Helix.Echo — 네이티브 DLL 없는 TCP echo를 실행한다.
- Helix.World — 델리게이트 조립과 인스턴스 격리를 시연한다.
- Helix.WorldServer — 네이티브 MMO 샘플과 같은 공통 Protobuf 메시지로 독립 월드를 실행한다.
- Helix.Tests — 기능 교체/격리/tick/불변 뷰/한도, TCP 프레이밍·종료·설정·외부 전송 주입을 검증한다.

## tools/ — 공통 실행 검증

- world_bot.py — 이동·필드 공격 거부·투기장 피해·인스턴스별 AOI를 프레임 프로토콜로 대조한다.
- verify_engines.py — 관리 테스트와 두 엔진의 echo/churn/MMO 샘플을 작업별 localhost 프로세스로 검증한다.


## 교체 직렬화 — 2026-10-12

- `cpp/Helix.Protocol/include/helix/protocol/messages.h`, `csharp/Helix.Protocol/Messages.cs`: 포맷 독립 메시지와 공통 의미 검증. 생성 타입과 Engine/Mmo에 의존하지 않음.
- `serializer.h` / `IPacketSerializer.cs`: IClientPacketSerializer/IServerPacketSerializer의 역할별 bytes ↔ DTO 계약. adapter는 양쪽 역할 IPacketSerializer, WorldApp은 서버 역할만 주입. `SerializerRegistry`는 조립 시 factory 등록/지원 검사, `WireSerializer`는 raw Protobuf 호환 및 선택 포맷 header 검사.
- `ProtobufSerializer.cpp` / `.cs`: 기존 `.proto` mapping. legacy Codec/PacketCodec·생성 타입 API는 보존.
- `cpp/Helix.Protocol.FlatBuffers`, `csharp/Helix.Protocol.FlatBuffers`: 선택 adapter, 운영 정본 `protos/helix_mmo.fbs`, 두 언어 verifier 뒤 소유 DTO 복원. 공개 C++ 헤더에 Google/플랫폼 타입 없음.
- `cmake/HelixFlatBuffers.cmake`: runtime/compiler 25.2.10 source 핀과 compiler 경로/자동 빌드. `tools/generate_serialization_schema.ps1`는 관리 Generated만 재생성.
- `csharp/Helix.Protocol.MemoryPack`: C# 전용 VersionTolerant DTO, 영구 order, field-length/array-count 사전 검사와 bounded formatter. 공통 DTO에 MemoryPack attribute를 넣지 않음.
- WorldServer 조립 지점이 registry로 codec을 선택해 WorldApp에 주입. EngineConfig `[protocol] serializer`는 이름만 저장하고 core는 codec을 생성하지 않음. 기존 MMO 규칙/네트워크 수명은 유지.
- `serializer_test.cpp`, `SerializerTests.cs`, `SerializedWorldClient.cs`, `tools/verify_engines.py`: 교차 fixtures + 실제 5개 서버/codec 조합 + 기존 Python Protobuf 경로. 세션 중 자동 협상은 없음.
- 관리 NuGet 0.4.0은 선택 두 패키지를 추가. C++ install은 `helix::protocol_flatbuffers`를 export. [wire 계약](SERIALIZATION.md).


## 실제 codec 측정 — 2026-10-12

- `csharp/benchmarks/Helix.WireBenchmarks`: 운영 registry/serializer 호출, DTO/입력 setup 및 round-trip, 메모리 내 TCP prefix wrapper, BDN EncodeFramed/DecodeValidated·MemoryDiagnoser. 정상 solution/패키지와 독립.
- `cpp/benchmarks/wire_serialization.cpp`: 같은 message 값과 실제 adapter + src/proto FrameCodec 헤더, 시간은 hook 없는 executable. `CountNew.cpp`는 별도 allocation probe만 링크하며 new 요청 bytes/calls를 집계.
- `tools/wire_native_benchmark.py`: 3회 순차 실행/포맷 순서 교대 및 타이밍·할당 CSV/metadata JSON 보존. `tools/wire_benchmark_report.py`: 54개 C# 조건/36개 native 조건·할당 안정성 검사 후 실제 수치·tradeoff 보고서 생성.
- 이 경로는 이전 bare-state benchmark와 별개다. [성능 측정 가이드](BENCHMARKS.md)의 재현 명령으로 `out/wire-report.md`에 생성하며 Git에는 포함하지 않는다. socket/프레임 수신 버퍼·tick은 제외하며 서로 다른 언어의 allocation 지표를 합치지 않는다.
