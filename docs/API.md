# API 레퍼런스

> 갱신: 2026-11-04. C++ SDK·선택 C ABI·순수 C# API는 별도 경계다. 선언의 정본은 각 공개 헤더와 `csharp/Helix.Engine/`다.

C++ 코어 SDK는 `#include "helix/helix.h"`로 사용한다. C ABI 소비자는 선택 빌드의 `helix_c.h`를 사용하며 C# 엔진은 이 ABI에 의존하지 않는다. 공개 C++ 헤더에는 `SOCKET`/`OVERLAPPED` 등 플랫폼 타입이 노출되지 않는다.

---

## C++ SDK

### `helix::IServerApp` — 구매자가 구현하는 유일한 핵심 인터페이스

```cpp
class IServerApp {
public:
    virtual void OnStart(Server& server) {}                       // 기동 직후 1회
    virtual void OnConnect(Session& s) {}                         // 새 연결
    virtual void OnMessage(Session& s, ByteSpan msg) = 0;         // 완결 메시지(필수)
    virtual void OnDisconnect(Session& s, SessionCloseReason r) {} // 종료
    virtual void OnDataVersion(std::string_view ch, uint64_t v) {}// 런타임 데이터 버전(scale-out)
    virtual void OnStop() {}                                      // graceful 종료 직전 1회
};
```

- **스레딩**: 콜백은 워커 스레드(TCP) 또는 전송 스레드(KCP)에서 호출된다. 동일 세션의 콜백은 순서가 보장된다.
- `msg`는 콜백 동안만 유효 — 보관하려면 복사할 것.
- `OnMessage`만 필수, 나머지는 선택 훅.

### `helix::Server` — 부트스트랩

```cpp
static Server Server::LoadConfig(std::string_view configPath);   // TOML+env 로드/검증
int  Run(std::unique_ptr<IServerApp> app);                       // Stop/시그널까지 블로킹, 종료코드 반환
void Stop();                                                     // 다른 스레드/시그널에서 graceful 종료
Session* FindSession(SessionId id) noexcept;                     // 활성 세션 조회(없으면 nullptr)
std::size_t SessionCount() const noexcept;

// --- scale-out (클러스터 비활성 시 로컬에 한해 동작) ---
Status RouteToSession(SessionId target, ByteSpan msg);           // 노드 투명 전송(로컬→직접, 타 노드→백플레인)
void   PublishDataVersion(std::string_view channel, uint64_t version);  // 전 노드 핫스왑 트리거
bool   ClusterKvSet(std::string_view key, std::string_view value, uint32_t ttlSec = 0);
std::optional<std::string> ClusterKvGet(std::string_view key);
```

### `helix::Session` — 연결 핸들 (엔진 소유)

```cpp
SessionId Id() const noexcept;
Status    Send(ByteSpan message);          // 프레이밍 자동, 세션별 직렬화 + 백프레셔
void      Close(SessionCloseReason r = SessionCloseReason::Kicked);
Endpoint  Remote() const;
TransportType Transport() const noexcept;  // Tcp/Kcp/Quic(예약 값)
void  SetUserData(void* p) noexcept;        // 앱 측 per-session 상태
void* GetUserData() const noexcept;
```

- 수명은 엔진이 소유한다. 콜백 밖에 원시 포인터를 보관하지 않고 `SessionId`를 저장한다. `FindSession`의 반환 포인터는 소유 참조가 아니므로 종료와 겹치는 장기 사용을 피한다. ID로 전송할 때는 `RouteToSession`을 사용한다.

### `helix::Snapshot<T>` — 런타임 데이터 핫스왑 (`runtime_data.h`)

불변 스냅샷 + atomic 포인터 교체로 무중단 실시간 갱신.

```cpp
helix::Snapshot<MyTable> g_table;

// 현재 게시 객체의 소유 참조를 읽는다.
auto snap = g_table.Load();        // std::shared_ptr<const MyTable>
if (snap) use(*snap);

// 쓰기(완성된 불변 스냅샷을 게시)
g_table.Store(std::make_shared<const MyTable>(...));
```

- 진행 중 reader는 옛 스냅샷을 끝까지 안전하게 사용(torn read 없음). 게시 후 스냅샷은 불변.
- `atomic<shared_ptr>`의 내부 구현이 lock-free라는 보장은 없다.
- scale-out 연계: `Server::PublishDataVersion(ch, v)` → 각 노드 `OnDataVersion`에서 재로딩 후 `Store`.

### 값 타입 (`types.h`)

| 타입 | 설명 |
|---|---|
| `SessionId` (`uint64`) | 세션 식별자. 상위 24비트=NodeId, 하위 40비트=노드 로컬 시퀀스. `MakeSessionId`/`NodeOf` 헬퍼 |
| `ByteSpan` (`std::span<const std::byte>`) | 소유권 없는 바이트 뷰 |
| `Status` | `Ok`/`ConfigError`/`AlreadyRunning`/`NotRunning`/`InvalidSession`/`BackpressureFull`/`TransportError`/`InternalError` |
| `SessionCloseReason` | `Normal`/`PeerClosed`/`IdleTimeout`/`IoError`/`ProtocolError`/`ServerShutdown`/`Kicked` |
| `TransportType` | `Tcp`/`Kcp`/`Quic`(미구현 예약 값) |
| `Endpoint` | `{ std::string ip; uint16_t port; }` |

### 최소 예제

```cpp
#include "helix/helix.h"
#include <csignal>

class Echo : public helix::IServerApp {
    void OnMessage(helix::Session& s, helix::ByteSpan m) override { s.Send(m); }
};

static helix::Server* g = nullptr;
int main() {
    auto server = helix::Server::LoadConfig("config/helix.toml");
    g = &server;
    std::signal(SIGINT, [](int){ if (g) g->Stop(); });
    return server.Run(std::make_unique<Echo>());
}
```

---

## C ABI (`helix_c.h`) — 선택 빌드

`HELIX_BUILD_CAPI=ON`으로 구성한다. C++ SDK를 감싸는 호환 경계이며 MMO 월드·Taskflow API를 노출하지 않는다.

규칙: 예외는 경계를 넘지 않는다(전부 `helix_status`). 객체는 불투명 핸들. 콜백의 `data`는 콜백 동안만 유효(복사 필요). ABI 호환성은 `helix_abi_version()`으로 확인.

```c
uint32_t helix_abi_version(void);                 /* == HELIX_ABI_VERSION(2) */

helix_status helix_server_create(const char* config_path, helix_server** out);
helix_status helix_server_set_callbacks(helix_server* s, helix_callbacks cb);
helix_status helix_server_run(helix_server* s);    /* 블로킹 */
helix_status helix_server_stop(helix_server* s);
void         helix_server_destroy(helix_server* s);

helix_status helix_session_send(helix_server* s, helix_session_id sid, helix_bytes msg);
helix_status helix_session_close(helix_server* s, helix_session_id sid, helix_close_reason r);
helix_status helix_session_remote(helix_server* s, helix_session_id sid,
                                  char* ip_buf, size_t ip_buf_len, uint16_t* port);
size_t       helix_server_session_count(helix_server* s);

/* --- scale-out (ABI v2) --- */
helix_status helix_session_route(helix_server* s, helix_session_id target, helix_bytes msg);
helix_status helix_cluster_publish_data_version(helix_server* s, const char* channel, uint64_t version);
helix_status helix_cluster_kv_set(helix_server* s, const char* key, helix_bytes value, uint32_t ttl_sec);
helix_status helix_cluster_kv_get(helix_server* s, const char* key,
                                  uint8_t* buf, size_t buf_len, size_t* out_len);  /* NOT_FOUND=키없음 */
```

`helix_callbacks`에 v2에서 `on_data_version`(nullable, 끝에 append)이 추가됐다. `helix_abi_version()` == 2.

콜백 등록:

```c
typedef struct {
    helix_on_connect_fn    on_connect;     /* nullable: (ctx, sid) */
    helix_on_message_fn    on_message;     /* required: (ctx, sid, bytes) */
    helix_on_disconnect_fn on_disconnect;  /* nullable: (ctx, sid, reason) */
    void*                  ctx;            /* 콜백마다 그대로 전달 */
    helix_on_data_version_fn on_data_version; /* v2, nullable: (ctx, channel, version) */
} helix_callbacks;
```

기본 `helix_status` 값 0..7은 `helix::Status`와 대응하며 C ABI에는 `HELIX_ERR_INVALID_ARG`와 `HELIX_ERR_NOT_FOUND`도 있다. `helix_close_reason`은 `helix::SessionCloseReason`과 대응한다. Windows DLL은 `helix.dll`, import lib은 `helix_capi.lib`다.

## 순수 C# (`Helix.Engine`)

`csharp/Helix.Engine`은 .NET 10 TCP 엔진이다. `HelixServer`는 `IServerTransport`, `Session`은 `ISessionTransport`에 의존한다. 네이티브 DLL이나 P/Invoke가 필요하지 않다.

```csharp
await using var server = HelixServer.Create("config/helix.toml");
Console.CancelKeyPress += (_, e) => { e.Cancel = true; server.Stop(); };
await server.RunAsync(new DelegateServerApp((session, message) =>
{
    if (!session.Send(message)) session.Close(CloseReason.IoError);
}));
```

예제에는 `using Helix.Engine;`이 필요하다. 직접 `IHelixApp`을 구현하거나 `DelegateServerApp`을 사용할 수 있다.

| API | 역할 |
|---|---|
| `HelixServer.Create(string path)` | 파일/env 설정으로 서버 생성 |
| `RunAsync(IHelixApp, CancellationToken)` | 한 서버 인스턴스를 한 번 실행 |
| `Stop()`, `DisposeAsync()` | 종료 요청과 정리 대기 |
| `FindSession(ulong)`, `SessionCount`, `Port`, `LastError` | 로컬 조회·상태 |
| `RouteToSession(ulong, ReadOnlySpan<byte>)` | **로컬** 세션으로 전송, 성공 여부 반환 |
| `Session.Send(ReadOnlySpan<byte>)`, `Close(CloseReason)` | 송신 접수 결과 반환·종료 요청 |

`Session`은 sealed class다. `RouteToSession`은 Redis 라우팅이 아니며 C#에는 KCP·Redis·TLS·C++ 병렬 월드의 동등 구현이 없다. 콜백 데이터의 보관이 필요하면 소유 복사본을 만든다.

## MMO 공개 경계

기본 C++ `helix::mmo`와 C# `Helix.Mmo`의 `WorldInstance`에는 이동·전투·가시성 정책을 주입한다. 입력은 `Queue*`, 상태 반영은 `Tick`, 교체는 `Replace*`, 조회는 완료 스냅샷을 사용한다. [조립 예제와 수명 계약](MMO.md)을 따른다.

C++ 선택 타깃 `helix::mmo_parallel`은 `helix/mmo/parallel_world.h`의 `ParallelWorldInstance`와 `helix/mmo/world_executor.h`의 `IWorldExecutor`를 제공한다. `QueueAreaAttack`과 공간 인덱스를 포함하며 기존 `WorldInstance`와 단계별 게임 의미가 다르다. 실행기는 `CreateSequentialWorldExecutor()` 또는 `CreateTaskflowWorldExecutor(workers)`로 주입하고 같은 실행기를 여러 월드가 공유할 수 있다. 동일 tick 내부의 서로 다른 엔티티 슬롯을 여러 worker가 갱신한다. 상세 [입력 순서·정책 동시성·예외·조회 계약](PARALLEL_MMO.md)을 확인한다.

---

## 관련 문서

- [CONFIGURATION.md](CONFIGURATION.md) — 설정 키 전체
- [BUILD.md](BUILD.md) — 독립 빌드·선택 모듈·검증
- [../HELIX_ENGINE_PLAN.md](../HELIX_ENGINE_PLAN.md) — 설계/로드맵
