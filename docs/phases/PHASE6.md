# Phase 6 — Scale-out (`IBackplane` + Redis 백플레인)

> 기록일: 2026-06-01 · 상태: **코드 완성 + 런타임 기동 검증됨**(Windows + Docker Redis, 2노드)
> 참고: Phase 4(Linux epoll)는 여전히 보류. 백플레인은 Windows에서 검증 가능.
>
> 검증 로그(2026-06-01): 노드0/1 모두 `backplane: redis connected 127.0.0.1:6379`,
> 채널 `helix:route:0` / `helix:route:1` 분리, `cluster=on`. 노드1 클라가
> `sid=1099511627777 = (1<<40)|1` 로 발급 → **SessionId에 NodeId 임베드 정상**.
> 빌드 이슈 2건 해결: (1) 정적 `helix.lib` ↔ DLL import lib 이름 충돌
> (→ `helix_capi` ARCHIVE_OUTPUT_NAME 분리), (2) 설정 파서가 인라인 주석 미제거
> (→ `ParseValueToken` 추가, 따옴표 존중 + `#`/`;` 제거).

## 목표
동질 노드 + L4 LB 구성에서 노드 간 협업(세션 디렉터리 · 크로스노드 라우팅 ·
공유 KV · 런타임 데이터 버전 브로드캐스트)을 Redis로 외부화한다. 계획서 2.4 / Phase 6.
외부 Redis 클라이언트 의존성 없이(자체 RESP 클라이언트) 신규 CMake 빌드를
의존성 0로 유지한다.

## 완료한 작업

### 자체 RESP 클라이언트 (의존성 0)
- `src/cluster/RespClient.{h,cpp}` — **신규**. 최소 RESP2 클라이언트(블로킹 WinSock).
  - 인코딩: 명령을 RESP 배열로. 디코딩: `+`단순/`-`에러/`:`정수/`$`벌크(바이너리 안전)/`*`배열/null.
  - `Command`(요청→응답) · `SendCommand`/`ReadReply`(파이프라인·구독 루프용).
  - 헤더에 WinSock 타입 비노출(소켓을 `std::uintptr_t`로 보관). TOML 파서와 같은 "직접 구현" 철학.

### Redis 백플레인 (`IBackplane` 구현)
- `src/cluster/RedisBackplane.{h,cpp}` — **신규**. 연결 2개:
  - **명령 연결**(`_cmd`, `_cmdMx`로 직렬화) — 디렉터리/라우팅 publish/KV/heartbeat.
  - **구독 연결**(`_sub`, 전용 스레드) — `SUBSCRIBE helix:route:{nodeId}` + `helix:dataver`, 끊기면 자동 재연결·재구독.
  - 세션 디렉터리: `SET helix:sess:{sid} {nodeId} EX ttl` / `GET`(Locate) / `DEL`. heartbeat가 로컬 보유 키 TTL을 파이프라인 `EXPIRE`로 갱신(좀비 정리).
  - 크로스노드 라우팅: `Locate(target)`→노드 → `PUBLISH helix:route:{node}` (8B sid + payload). 수신측 구독 스레드가 디코딩 → `IBackplaneSink::OnRouted`.
  - 데이터 버전: `PUBLISH helix:dataver` (8B version + channel) → 전 노드 `OnDataVersion`.
  - 공유 KV: `SET/GET helix:kv:{key}`.
- `src/cluster/BackplaneFactory.{h,cpp}` — **신규**. `CreateBackplane(BackplaneKind)`(`none|redis`). 코어는 `IBackplane`만 의존(DIP), NATS/Kafka 추가는 구현+분기만(OCP).

### SessionId 전역 유일화 + 옵저버
- `include/helix/types.h` — **수정**: `MakeSessionId(nodeId, seq)` / `NodeOf(id)`. 상위 24비트=NodeId, 하위 40비트=노드 로컬 시퀀스. **디렉터리 키 충돌 방지 + 라우팅 대상 노드를 ID에서 도출**. `nodeId=0`이면 과거와 동일(1,2,3…) → 하위호환.
- `src/session/ISessionObserver.h` — **신규**: 세션 수명 알림(ISP). `SessionManager`는 수명/조회만 책임지고(SRP), 디렉터리 등록은 옵저버로 위임.
- `src/session/SessionManager.{h,cpp}` — **수정**: 생성자에 `nodeId`+`observer`(둘 다 기본값 → 단일 노드 무변경). `Create`가 `MakeSessionId`로 ID 합성 + 맵 락 밖에서 `OnSessionOpened`. `Remove`가 락 밖에서 `OnSessionClosed`.

### 코어/공개 API 연동
- `src/core/ServerCore.{h,cpp}` — **수정**: `IBackplaneSink`+`ISessionObserver` 구현.
  - `ServerCoreConfig`에 cluster 필드(enabled/backend/url/nodeId/sessionTtlSec/heartbeatMs).
  - `Start`: 클러스터면 백플레인 생성·Connect(실패 시 경고 후 단일 노드로 계속), `SessionManager`에 nodeId+observer 주입, heartbeat 스레드 기동.
  - `OnSessionOpened/Closed` → 백플레인 Register/Unregister. `OnRouted` → `Find(target)->Send`. `OnDataVersion` → `IServerApp::OnDataVersion`.
  - `RouteToSession`(로컬 우선, 타 노드는 백플레인) / `PublishDataVersion` / `KvSet`/`KvGet`.
  - 종료: 구독 스레드를 **세션 정리보다 먼저** join(파괴된 SessionManager 접근 차단).
- `include/helix/server_app.h` — **수정**: `OnDataVersion(channel, version)` 선택 훅 추가.
- `include/helix/server.h` + `src/core/Server.cpp` — **수정**: `RouteToSession`/`PublishDataVersion`/`ClusterKvSet`/`ClusterKvGet` 공개 + cluster 설정 매핑.

### 설정 / 빌드 / 검증
- `src/config/EngineConfig.{h,cpp}` — **수정**: `[cluster]` 로드 + 검증(backend=redis, node_id<2^24, heartbeat_ms<ttl*1000 등).
- `config/helix.toml` — **수정**: `[cluster]` 섹션(기본 `enabled=false`).
- `CMakeLists.txt` — **수정**: cluster 소스 3개 추가(static+shared 공용), `helix_route_phase6` 샘플 타깃.
- `samples/route_phase6/main.cpp` — **신규**: 접속 시 클라에 전역 SessionId 통지, `[8B 대상 sid][payload]` 수신 시 `RouteToSession`. 같은 바이너리를 여러 노드로 띄워 노드 투명 라우팅 시연.
- `tools/cluster_route_bot.py` — **신규**: 같은 노드 + 노드 간 라우팅 왕복 검증, PASS/FAIL.

## 설계 결정
- **자체 RESP 클라이언트** — vcpkg 의존성 0 유지 + 풀 컨트롤(재연결/파이프라인). hiredis/redis-plus-plus 미사용(사용자 결정).
- **SessionId에 NodeId 임베드** — 디렉터리 키 전역 유일성을 ID 수준에서 보장. 단일 노드 하위호환(nodeId=0).
- **연결 2개(명령/구독)** — 구독은 블로킹 전용 스레드, 명령은 mutex 직렬화. 단순·정확.
- **로컬 우선 라우팅** — 같은 노드면 Redis 왕복 없이 직접 송신. 타 노드만 pub/sub(at-most-once, 계획서 5절 합의).
- **백플레인 실패 = 단일 노드로 폴백** — Redis 미가용 시 서버는 죽지 않고 standalone로 계속.

## 미완료 / 다음 단계로 이월
- **디렉터리 등록이 accept/워커 스레드에서 동기 Redis 호출** — 고churn에서 `_cmdMx`+RT로 직렬화. 후속: 전용 큐로 비동기 배치(노드 해시 + 단일 `EXPIRE`).
- **C ABI/C# 노출 없음** — `RouteToSession`/KV/데이터버전은 현재 C++ 전용. helix_c.h 확장은 후속(Phase 8 연계).
- pub/sub 전달 보장은 at-most-once — 신뢰 필요 시 ack/재전송 또는 스트림(XADD) 검토.
- Phase 4(Linux epoll) 보류 지속 — RESP 클라이언트는 BSD 소켓 경로만 추가하면 이식 가능하도록 `#if _WIN32` 경계 둠.

## 검증 상태
**코드 완성 / 런타임 검증 대기.** 절차(Windows + Redis):
1. Redis 기동(127.0.0.1:6379).
2. 빌드: `cmake --preset windows-x64` → `cmake --build --preset windows-debug`.
3. 2노드 기동(작업 디렉터리에 `config/helix.toml` 필요):
   - 노드0: `set HELIX_CLUSTER__ENABLED=true && set HELIX_CLUSTER__NODE_ID=0 && set HELIX_NETWORK__PORT=5000 && helix_route_phase6`
   - 노드1: `set HELIX_CLUSTER__ENABLED=true && set HELIX_CLUSTER__NODE_ID=1 && set HELIX_NETWORK__PORT=5001 && helix_route_phase6`
4. `python tools/cluster_route_bot.py --ports 5000 5001` → 같은 노드 + 노드 간 양방향 라우팅 **PASS** 확인.
   - Redis 없이 1노드만 띄우고 `--ports 5000` 이면 같은 노드 라우팅만 PASS(클러스터 off여도 동작).
5. 좀비 정리: 노드 강제 종료 후 `redis-cli keys "helix:sess:*"` 가 `session_ttl_sec` 후 비워지는지 확인.

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md (2.4 백플레인, 5 리스크)
- 이전 단계: ./PHASE5.md
