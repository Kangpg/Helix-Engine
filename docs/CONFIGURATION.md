# 설정 레퍼런스

> 갱신: 2026-11-04. 범용 엔진의 연결·버퍼·프레이밍·로그 설정과 선택 모듈의 설정을 설명한다. 현재 지원 범위는 [README](../README.md)를 따른다. C#에는 platform backend 설정이 적용되지 않으며 KCP/cluster와 C++ 병렬 실행 설정은 미지원이다.

Helix는 **코드 밖 설정**을 원칙으로 한다. 레이어링(12-factor):

```
코드 기본값  →  TOML 파일  →  환경변수 오버라이드
```

- 파일은 `Server::LoadConfig("config/helix.toml")`에 넘긴 경로. 파일이 없으면 기본값으로 진행(에러 아님).
- 환경변수 규칙: **`HELIX_<SECTION>__<KEY>`** (대문자, 구분자는 언더스코어 **2개**).
  - 예: `HELIX_NETWORK__PORT=6000`, `HELIX_LOGGING__LEVEL=debug`, `HELIX_CLUSTER__ENABLED=true`
- 파서는 TOML/INI 공통 부분집합(`[section]` + `key = value`, `#`/`;` 주석, 인라인 주석 제거, 따옴표 해제)을 지원.
- 기동 시 **검증**한다. 실패하면 서버는 시작을 거부하고 `Status::ConfigError`로 종료(사유 로그).

bool 표기: `true/false`, `1/0`, `yes/no`, `on/off` 모두 허용.

---

## `[network]`

| 키 | 기본값 | 설명 |
|---|---|---|
| `ip` | `"0.0.0.0"` | 바인드 주소 |
| `port` | `5000` | 바인드 포트 (1..65535) |
| `transport` | `"tcp"` | `tcp` \| `kcp` \| `quic`(예정). 전송 계층 선택 |
| `backend` | `"auto"` | `auto` \| `iocp` \| `iocp_rio` \| `epoll`. **C++ TCP 전용**, Windows IOCP/RIO·Linux epoll |
| `max_connections` | `10000` | 최대 동시 연결 |
| `backlog` | `128` | listen 백로그 (TCP) |
| `io_threads` | `0` | I/O 워커 스레드 수, `0`=CPU 코어 수 (TCP) |
| `concurrent_accepts` | `16` | 사전 게시 AcceptEx 수 (TCP) |

**transport 선택**

- `tcp` — 기본. C++ `INetBackend`(Windows IOCP/RIO·Linux epoll) 또는 C# async TCP.
- `kcp` — C++ 신뢰 UDP. `ThirdParty/kcp/ikcp.{h,c}`는 저장소에 포함되어 있다. `backend`/`io_threads` 등 TCP 전용 키는 무시되고 KCP가 자체 UDP I/O를 돌린다.
- `quic` — 예정(미구현). 지정 시 기동 실패.

**backend(`auto`) 동작** — Windows auto/iocp는 일반 IOCP, Linux auto는 epoll이다. iocp_rio는 IOCP AcceptEx + 실제 RIO 송수신이며 초기화 실패를 기동 실패로 보고한다. C#은 자체 async TCP 경로라 RIO를 사용하지 않는다.

## `[rio]` — Windows C++ TCP

| 키 | 기본값 | 설명 |
|---|---:|---|
| `buffer_size` | `16384` | 컨텍스트별 등록 버퍼 크기, 양수 |
| `completion_queue_size` | `32768` | CQ 크기, 양수. RIO 선택 시 `max_connections * 2` 이상 |

`HELIX_RIO__*`로 덮어쓴다. 큰 송신은 여러 chunk로 이어서 전송한다.

## `[protocol]`

`serializer="protobuf"`가 기본이며 `HELIX_PROTOCOL__SERIALIZER`로 덮어쓴다. C++/C# 공통 `flatbuffers`, C# 전용 `memorypack`을 선택할 수 있다. 이름은 EngineConfig에 저장하고 지원 여부는 앱의 SerializerRegistry에서 검사한다. 기동 시 고정하며 미등록 이름은 기동 실패다. Engine 바이트 framing과 별개이고 [wire/호환 규칙](SERIALIZATION.md)을 따른다.

## `[buffers]`

| 키 | 기본값 | 설명 |
|---|---|---|
| `recv_buffer_size` | `8192` | 세션당 수신 버퍼 바이트 |
| `max_send_queue` | `1024` | 세션당 송신 대기 한계. 초과 시 `Send`가 `Status::BackpressureFull` |

## `[framing]`

TCP/KCP 모두 **길이 프리픽스 프레이밍**으로 메시지 경계를 만든다.

| 키 | 기본값 | 설명 |
|---|---|---|
| `header_size` | `4` | 길이 헤더 바이트. `2` 또는 `4` |
| `big_endian` | `true` | 헤더 바이트 순서(네트워크 = big-endian) |
| `max_message_size` | `1048576` | 최대 메시지 바이트(과대/손상 패킷 가드). 초과 시 `ProtocolError`로 연결 종료 |

> 검증 봇(`tools/echo_bot.py`)은 기본 4바이트 big-endian과 일치. 다르게 설정하면 `--header-size 2` / `--little`로 맞출 것.

## `[logging]`

| 키 | 기본값 | 설명 |
|---|---|---|
| `level` | `"info"` | `debug` \| `info` \| `warn` \| `error` |
| `sink` | `"stdout"` | `stdout` \| `file` \| `both` (컨테이너는 `stdout` 권장) |
| `file` | `"helix.log"` | `sink`에 file 포함 시 로그 경로 |

## `[shutdown]`

| 키 | 기본값 | 설명 |
|---|---|---|
| `drain_timeout_ms` | `5000` | graceful 종료 예산(ms). native Windows는 초과 시 경고하고 세션/AcceptEx 취소 완료까지 소유자를 보존한다. 강제 deadline이 아니다. Linux는 기존 timeout 경로 |

## `[cluster]` — scale-out (선택)

기본 비활성(단일 노드). 활성 시 Redis 백플레인으로 노드 간 협업.

| 키 | 기본값 | 설명 |
|---|---|---|
| `enabled` | `false` | `true`면 백플레인 사용 |
| `backend` | `"redis"` | 현재 `redis`만 |
| `url` | `"redis://127.0.0.1:6379/0"` | Redis 접속 URL(`redis://host:port/db`) |
| `node_id` | `0` | 노드별 고유값(< 16777216). **SessionId 상위 24비트에 임베드**(전역 유일성·라우팅 대상 도출) |
| `session_ttl_sec` | `30` | 세션 디렉터리 엔트리 TTL(좀비 정리) |
| `heartbeat_ms` | `10000` | 디렉터리 TTL 갱신 주기. `session_ttl_sec*1000` 보다 작아야 함 |

- Redis 연결 실패 시 서버는 죽지 않고 **단일 노드로 폴백**한다.
- `node_id`는 노드마다 달라야 한다(같으면 SessionId 충돌). 다중 노드 예:
  ```
  노드0: HELIX_CLUSTER__ENABLED=true HELIX_CLUSTER__NODE_ID=0 HELIX_NETWORK__PORT=5000
  노드1: HELIX_CLUSTER__ENABLED=true HELIX_CLUSTER__NODE_ID=1 HELIX_NETWORK__PORT=5001
  ```
- 크로스노드 전달은 `Server::RouteToSession(sid, msg)`, 데이터 버전 브로드캐스트는 `Server::PublishDataVersion(...)`, 공유 KV는 `Server::ClusterKvSet/Get`. 자세한 건 [API.md](API.md).

## 선택 콘텐츠: `[mmo]` 실행 한도

`[mmo]`의 `max_entities=10000`, `max_pending_commands=4096`, `max_commands_per_tick=1024`, `tick_ms=50`을 사용한다. 각각 `HELIX_MMO__MAX_ENTITIES`, `HELIX_MMO__MAX_PENDING_COMMANDS`, `HELIX_MMO__MAX_COMMANDS_PER_TICK`, `HELIX_MMO__TICK_MS`로 덮어쓴다. 모두 양수이며 잘못된 MMO 정수값은 로드 오류다. C++ `EngineConfig`와 C# `EngineConfig`는 동일 키를 사용하고 WorldLimits는 앱에서 이 값으로 구성한다.

C++ 전용 선택 병렬 실험은 다음 키를 추가한다. 동일한 `HELIX_MMO__*` 규칙으로 덮어쓴다.

| 키 | 기본값 | 의미·검증 |
|---|---:|---|
| `execution` | `"sequential"` | `sequential` 또는 `parallel`. C++ 월드 샘플의 조립에서 소비 |
| `worker_threads` | `4` | 프로세스에서 공유하는 콘텐츠 worker 수, 양수. I/O worker와 별도 |
| `batch_size` | `128` | 엔티티 배치 크기, 양수 |
| `cell_size` | `16.0` | XZ 셀 크기, 유한 양수 |
| `visibility_radius` | `50.0` | XYZ 거리 AOI 반경, 유한 0 이상 |
| `max_hits_per_tick` | `65536` | 단일·광역 피격 이벤트 계획 예산, 양수 |

잘못된 값은 C++ 설정 로드 오류다. `parallel`은 `HELIX_BUILD_PARALLEL_MMO=ON` 빌드를 요구하며 pool 구성은 기동 후 hot reload하지 않는다. 기본 C++ `WorldInstance`와 C# 실행 모드를 변경하는 값이 아니다. [단계별 의미와 소비 지점](PARALLEL_MMO.md)을 참고한다.

## `[quic]` — 예정

QUIC(msquic) 도입 시 사용할 섹션(현재 미구현). 예정 키: `cert_file`, `key_file`, `alpn`.

---

## TCP·클러스터 설정 예시

아래는 일부 섹션의 예시다. `[mmo]`·`[protocol]`·`[rio]`를 포함한 전체 기본 설정은 추적 파일 [config/helix.toml](../config/helix.toml)을 기준으로 한다.

```toml
[network]
ip = "0.0.0.0"
port = 5000
transport = "tcp"          # tcp | kcp
backend = "auto"           # auto | iocp | iocp_rio
max_connections = 10000
backlog = 128
io_threads = 0
concurrent_accepts = 16

[buffers]
recv_buffer_size = 8192
max_send_queue = 1024

[framing]
header_size = 4
big_endian = true
max_message_size = 1048576

[logging]
level = "info"
sink = "stdout"
file = "helix.log"

[shutdown]
drain_timeout_ms = 5000

[cluster]
enabled = false
backend = "redis"
url = "redis://127.0.0.1:6379/0"
node_id = 0
session_ttl_sec = 30
heartbeat_ms = 10000
```

배포 시: 이미지에는 기본값을 베이크하고, 배포별 값(포트/노드ID/Redis URL 등)은 환경변수로 주입한다.
