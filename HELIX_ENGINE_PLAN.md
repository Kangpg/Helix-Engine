# Helix 서버 엔진 재설계 계획 (v2)

> 목표: **범용 멀티전송 서버 엔진**을 만들어, 일반 업체가 구매 후 저수준 네트워크를 몰라도 핸들러만 구현하면 동작하도록 한다. 모든 설정은 코드 밖(파일 + 환경변수)으로 빼고, 컨테이너로 배포하며, 노드를 실시간으로 가로 확장(scale-out)할 수 있어야 한다.

## 현재 방향 — 2026-11-04 갱신

이 절은 아래 v2 계획의 언어/전송 우선순위보다 우선한다. 기존 단계 기록은 당시 구현의 이력으로 유지한다.

- **C++과 C# 독립 엔진**: `cpp/CMakeLists.txt`와 `csharp/Helix.sln`을 각각 빌드한다. C#은 순수 .NET TCP 엔진으로 실행하며 P/Invoke/네이티브 DLL에 의존하지 않는다. C++ C ABI는 호환용 선택 빌드(`HELIX_BUILD_CAPI=ON`)로 유지한다.
- **MMO 모듈은 네트워크와 분리**: 양쪽 `Helix.Mmo`는 인스턴스별 `IMovementPolicy`/`ICombatRules`/`IVisibilityPolicy`를 주입받는다. 클래스와 델리게이트 어댑터가 같은 계약을 구현한다.
- **상태 변경의 실행 계약**: 기존 C++/C# `WorldInstance`는 단일 tick writer다. C++ 선택 `ParallelWorldInstance`는 한 tick의 작업 그래프 안에서 서로 다른 엔티티 슬롯을 여러 worker가 동시에 갱신한다. tick 조정은 중첩 실행을 거부하며 입력·정책은 시작 경계에서 포착하고 완료 상태만 불변 게시한다. [단계별 전투 의미와 검증](docs/PARALLEL_MMO.md)을 따른다. C#에 같은 구조를 함께 구현하는 범위가 아니다.
- **QUIC 후순위**: C++ 기존 TCP/KCP를 유지한다. 순수 C# 1차 범위는 TCP이며 KCP/Redis 구현은 별도 후속이다. 기존 QUIC 예약 값은 동작 구현을 뜻하지 않는다.
- **Phase 10 — MMO 인스턴스 조립 및 언어별 프로젝트 분리**: 독립 빌드, 기능 교체, 순수 C# TCP, 공통 샘플 프로토콜과 검증을 기록한다. C++ 선택 병렬 실험에는 공간 인덱스를 추가했으며 기존 C++/C# `WorldInstance`에는 적용하지 않았다. 대규모 부하 최적화·월드 마이그레이션은 후속 범위다.
- **C++ 병렬 MMO 실험**: Taskflow 4.1.0을 private adapter로 사용하는 `helix::mmo_parallel`을 기본 OFF로 제공한다. Windows 정확성·실제 통신·설치 소비자 검증은 통과했으나 현재 단순 규칙의 inline 대비 성능 개선은 확인하지 못했다. 다음 작업은 단계별 시간·할당 계측과 실제 콘텐츠 부하 비교다. AOI/패킷 생성 병렬화·Linux 실행·운영 수용 인원 검증은 남아 있다. Redis 보강·Kafka 도입은 이번 실험에 포함하지 않았다.
- **공통 메시지 추가**: `protos/helix_mmo.proto`와 양쪽 독립 `Helix.Protocol`에서 Protobuf를 생성한다. 직렬화는 MMO 앱 계층에 조립하며 Engine/Mmo는 바이트·도메인 계약을 유지한다. Asio는 도입하지 않고 현재 C++ IOCP/epoll을 유지한다. Phase 10 추가 작업으로 기록한다.
- **직렬화 비교 및 RIO 추가**: 운영 메시지는 Protobuf로 유지하면서 독립 benchmark에서 C# 6포맷, C++ Protobuf/FlatBuffers/Binary를 비교한다. Windows `iocp_rio`는 IOCP AcceptEx/완료 통지 + 실제 RIO 송수신으로 구현한다. `auto`는 IOCP이고 명시적 RIO 초기화 실패는 시작 실패로 보고한다. 과거 v2의 자동 폴백/미구현 설명은 현재 계약보다 우선하지 않는다. 성능 판단은 동일 Release/부하의 측정에 근거한다.
- **.NET 10 전환**: 관리 엔진·MMO·Protocol·샘플·테스트·benchmark는 net10.0, stable SDK 10 선택, CI 10.0.x로 맞춘다. 관리 패키지는 0.3.0으로 구분하고 .NET 8 결과/0.2.0 패키지 이력을 유지한다. 직렬화는 같은 조건으로 재측정하고 실제 결과·크기·할당·언어 경계를 근거로 결론을 기록한다.

- **직렬화 주입 및 호환**: 기본 raw Protobuf 유지. 포맷 독립 DTO + IPacketSerializer + registry factory로 교체하며 C++/C# 공통 FlatBuffers, C# 전용 MemoryPack을 선택 모듈로 제공한다. `[protocol] serializer`/env는 기동 시 선택, 연결 중 고정이다. 포맷 메타데이터·공통 의미 검증·교차 언어 fixtures 및 실제 MMO 통신으로 검증한다. 관리 패키지 0.4.0, 이전 0.3.0 이력 보존. 세션 중 자동 협상/새 adapter 성능 최적화는 후속이다.

- **클라이언트 엔진 독립**: Unity 또는 Unreal을 공통 대상으로 가정한다. C++·C# 서버는 각각 독립이며 외부 wire는 같은 schema/버전/framing을 쓴다. 공통 Protobuf 기본·FlatBuffers 선택, MemoryPack은 C# 전용 경로다. 현재 서버 .NET 10 패키지와 client SDK를 구분하며 Unity IL2CPP/Unreal 빌드 호환·성능은 별도 검증한다. 기존 언어별 benchmark 값은 보존하고 특정 언어 조합의 산술 시간을 게임 클라이언트 성능으로 쓰지 않는다.

## 초기 v2 설계 자료 (이력 보존)

이하의 분석·목표·로드맵은 재설계 착수 당시 자료다. 현재 구현이나 지원 목록이 아니며, 위 최신 방향과 [README](README.md)·[Phase 기록](docs/phases/)을 우선한다. 예전 C# P/Invoke 구조, QUIC 계획, RIO 미구현과 Phase 0 착수 설명을 현재 작업 지침으로 적용하지 않는다.

### 당시 확정한 방향

| 항목 | 결정 |
|---|---|
| 플랫폼 | **Windows + Linux 컨테이너 둘 다** (백엔드 추상화 `INetBackend`로 IOCP/RIO + epoll) |
| 서버 성격 | **범용 엔진** — 프로토콜은 구매 업체가 정의, 엔진은 바이트 스트림 + 프레이밍 제공 |
| 전송(transport) | **TCP + KCP + QUIC** (`ITransport` 추상화로 선택 가능) |
| 설정 | **TOML 파일(기본값) + 환경변수 오버라이드** (12-factor), 기동 시 검증, 일부 핫리로드 |
| 배포 | **Docker** (멀티스테이지, 비루트, stdout 구조화 로그, SIGTERM 드레이닝) |
| Scale-out | **동질 노드 + Redis 백플레인** (세션 디렉터리 + pub/sub + 공유 상태), L4 LB |
| 다언어 지원 | **안정적 C ABI 코어 + C++/C# 래퍼** (msquic·GNS 방식) |
| 런타임 데이터 | **불변 스냅샷 + atomic 포인터 교체**로 무중단 실시간 갱신 |

---

## 1. 현재 구조 분석

### 1.1 구성
- **HelixCore** (정적 라이브러리): 저수준 네트워크 프리미티브
- **GameServer** (실행 파일): HelixCore를 엮어 워커 스레드를 돌리는 얇은 호스트
- 빌드는 VS 솔루션, 의존성은 vcpkg manifest(inih·curl·mimalloc·grpc), `ThirdParty/`에 inih·mimalloc 서브모듈.

### 1.2 파일별 현황

| 파일 | 역할 | 상태 |
|---|---|---|
| `Types.h` / `Noncopyable.*` | 타입 별칭 / 복사 금지 믹스인 | 완성 |
| `Logger.*` | 파일 로거 | **미완성** (1.4 참조) |
| `Network.*` | WSAStartup/Cleanup 래퍼 | 동작하나 설계 결함(헤더 전역 인스턴스) |
| `INetwork.h` | 인터페이스 선언 | **선언만, 구현 없음** + `abstract`(MSVC 전용) |
| `Iocp.*` | IOCP 핸들 래퍼 | 비교적 완성 |
| `SocketExtension.*` | AcceptEx/RIO 함수 포인터 로더 | 완성 |
| `RioBufferPool.*` | RIO 등록 버퍼 풀 | 구현됨 |
| `HybridNetworkRuntime.*` | IOCP + (옵션)RIO 결합 | 초기화/종료만, 실제 I/O 게시 없음 |
| `ServerConfig.*` + `INIData.*` + 로더들 | INI 설정 로딩 | 동작 |
| `Server.h`(Core) / `GameServer.*` | 추상 서버 / 워커 루프 | 골격 (완료를 로그만 함) |
| `main.cpp` | 부트스트랩 | 동작 |

### 1.3 가장 중요한 결손 — "아직 연결을 못 받는다"
1. **Accept/Listen이 없다** — `IAcceptor`는 선언만, listen·bind·AcceptEx가 미구현. 즉 클라이언트가 접속 불가. **1순위.**
2. **Session/Connection 관리가 없다** — 세션 풀·ID·수명관리(close 레이스) 없음.
3. **실제 recv/send가 없다** — RIO 버퍼풀은 있으나 I/O 게시 없음. `HandleCompletion`은 로그만 함.
4. **프레이밍/패킷 계층이 없다** — TCP 스트림에서 메시지 경계를 만드는 길이 프리픽스 부재.
5. **핸들러 디스패치가 없다** — 조립된 메시지를 앱으로 전달하는 경로 없음.
6. **구매자용 API 경계(SDK)가 없다** — 저수준을 직접 만져야 함.

### 1.4 고쳐야 할 기존 결함
- **Logger**: `Log()`가 레벨/타임스탬프를 무시(함수명·라인만 기록)하고, 쓰기 경로에 **락이 없어 스레드 안전하지 않음.** flush·로테이션·레벨 필터·비동기 없음.
- **Network.h 전역 인스턴스**: 헤더의 `static` 전역 → TU마다 별도 인스턴스, `WSAStartup` 중복 위험. 단일 소유화.
- **`abstract` 키워드**: MS 전용. Linux 이식 걸림돌 → `= 0`로 교체.
- **README 불일치**: CMakeLists/프리셋/설정 파일명이 실제와 다름. 빌드 일원화 필요.
- **`#define RPMALLOC_ENABLE_OVERRIDE`**: mimalloc 사용 중인데 rpmalloc 매크로 — 혼선, 제거.
- **하드코딩 값**: 프레이밍 파라미터, accept 동시 게시 수, RIO 완료 배열(64), 소켓 옵션, 로그 레벨/경로 등 외부화 안 됨.

### 1.5 살릴 자산
`Iocp`, `SocketExtension`, `RioBufferPool`, 설정 로딩 골격, 타입/믹스인은 재사용 가치 높음. "버리고 새로"가 아니라 **이 위에 누락 계층을 올리고 추상화·설정·배포를 현대화**하는 방향.

---

## 2. 목표 아키텍처

핵심 원칙: 모든 확장축(플랫폼·전송·언어·노드)이 **"안정적 C ABI 코어 + 추상화 경계 3개"** 위에서 풀린다. 이 경계만 초기에 제대로 잡으면 나머지는 모듈 추가로 끼운다.

```
┌──────────────────────────────────────────────────────────────┐
│  구매 업체 코드                                                 │
│   [C++ SDK]  IServerApp: OnConnect/OnMessage/OnDisconnect      │
│   [C#  SDK]  동일 핸들러 모델 (P/Invoke 래퍼)                   │
└───────────────▲──────────── 공개 SDK ──────────────────────────┘
                │
┌───────────────┴──────────── 안정적 C ABI ──────────────────────┐
│   helix_*  (불투명 핸들, 에러코드, 콜백). 저수준 타입 비노출      │
│   → C++/C# 래퍼가 이 위를 얇게 감쌈 (단일 코어 유지보수)          │
└───────────────▲────────────────────────────────────────────────┘
┌───────────────┴────────────── 엔진 코어 (C++) ──────────────────┐
│  Bootstrap · Acceptor · SessionManager · IO Engine · Framing    │
│  Dispatcher · Worker(Job) · Timer · Logger · Stats              │
│  RuntimeData(불변 스냅샷 + atomic 스왑)                          │
└──▲──────────────────▲────────────────────▲─────────────────────┘
   │ INetBackend       │ ITransport          │ IBackplane
┌──┴────────────┐ ┌────┴──────────────┐ ┌────┴───────────────────┐
│ [Win] IOCP/RIO│ │ TCP · KCP · QUIC  │ │ [기본] Redis            │
│ [Linux] epoll │ │ (QUIC=msquic 자체 │ │  세션디렉터리/pub-sub/  │
│               │ │  I/O)             │ │  공유상태               │
└───────────────┘ └───────────────────┘ └─────────────────────────┘
```

### 2.1 안정적 C ABI 코어 (다언어의 핵심)
엔진 코어는 C++로 두되 **공개 경계를 C ABI로 노출**한다(`helix_server_create`, `helix_session_send`, 콜백 등록 등 — 불투명 핸들 + 에러코드). C++ SDK와 C# SDK(P/Invoke)는 이 위를 감싸는 얇은 래퍼. 유지보수 대상이 코어 하나로 고정되고 ABI 안정성을 얻는다. msquic·GameNetworkingSockets가 쓰는 검증된 방식.
- 규칙: **예외는 ABI를 넘지 않는다**(전부 에러코드), 핸들 수명은 명시적, 콜백 스레딩은 문서화(C#은 GC 핀닝/델리게이트 수명 주의).

### 2.2 플랫폼 백엔드 (`INetBackend`)
IOCP=proactor, epoll=reactor. **proactor 스타일로 인터페이스 통일**하고 Linux는 내부에서 epoll→proactor 에뮬레이션. 코어는 백엔드를 모른다. 기존 `Iocp`/`SocketExtension`/`RioBufferPool`를 Windows 구현으로 이식.

**백엔드는 설정으로 고른다(`IoBackendKind`).** 일반 IOCP만 강제하지 않고 사용자가 선택한다:

| 설정값 | 동작 |
|---|---|
| `auto` (기본) | OS에 맞춰 자동 선택 — Windows는 RIO 가용 시 `iocp+rio`, 아니면 `iocp` / Linux는 `epoll` |
| `iocp` | 일반 IOCP. 호환·단순, 중간 부하에 충분 |
| `iocp_rio` | IOCP + Registered I/O 고속 경로(Windows 8/Server2012+). 고동접·고패킷레이트에서 처리량↑·CPU↓ |
| `epoll` | Linux |

규칙: **능력 탐지 + 폴백.** `iocp_rio`를 골라도 RIO 확장이 없으면 경고 로그 후 일반 IOCP로 자동 폴백한다(기존 폴백 로직 재사용). RIO는 Windows 전용 개념이라 Linux에선 무시되고 epoll로 간다. `Name()`이 실제 활성 백엔드를 보고한다(폴백 시 `iocp`).

트레이드오프: RIO는 커널 전환·연결당 오버헤드를 줄여 고밀도 동접에 유리하나, 버퍼를 미리 핀(pin)해 메모리를 선점하고 복잡도·디버깅 난도가 높다. 그래서 **기본은 `iocp`(또는 `auto`), `iocp_rio`는 opt-in**. 실제 RIO 데이터 경로는 Phase 4에서 정식 구현하고, Phase 1은 IOCP로 통신부터 뚫는다.

### 2.3 전송 추상화 (`ITransport`) — TCP / KCP / QUIC
세션·프레이밍 위에서 전송을 갈아끼운다.
- **TCP**: 기본. `INetBackend`(IOCP/epoll)의 raw 소켓 사용.
- **KCP**: 저지연 신뢰 UDP. `ikcp.c`(원본 C) 임베드 — C#의 kcp2k와 같은 뿌리. UDP 소켓은 `INetBackend` 위에. 게임형 워크로드에 적합(지연↓, 대역폭 10~20%↑).
- **QUIC**: msquic 래핑. 신뢰·멀티플렉싱·TLS1.3·0-RTT 내장, **크로스플랫폼(Win=RIO/IOCP, Linux=epoll)**. 단 msquic은 자체 I/O를 가지므로 `INetBackend` 아래가 아니라 **별도 전송으로 병렬 배치**(이 비대칭을 인터페이스에서 흡수).

**기본값은 TCP다.** 전송을 지정하지 않으면 일반 TCP 서버로 동작하며, KCP/QUIC는 선택형 add-on이다. Phase 1 MVP도 TCP로 출발한다.

#### 전송 선택 가이드 (게임/워크로드 유형별)

| 유형 | 권장 전송 | 이유 |
|---|---|---|
| 테마파크/탭타겟 MMO, 턴제, 일반 서버 | **TCP** (기본) | 검증·단순, 순서/신뢰 보장, **고밀도 연결당 비용 최저**(MMO 동접 밀도에 유리). WoW·FFXIV류가 TCP |
| 액션 MMO / 고틱레이트 | **KCP** | 저지연 신뢰 UDP, 신뢰+비신뢰 채널 분리, 작고 잦은 패킷에 최적 |
| FPS / 실시간 대전 / MOBA | **KCP** (또는 커스텀 UDP) | TCP HoL 회피, 지연 민감 워크로드 |
| 모바일(네트워크 전환 잦음) / 보안 중시 | **QUIC** | 연결 마이그레이션(wifi↔셀룰러 유지), TLS1.3 내장 |
| 채널 분리(채팅/이동/인벤토리) 필요 | **QUIC** | 스트림 멀티플렉싱(스트림 간 HoL 제거), 비신뢰는 DATAGRAM |

> 주의: QUIC는 연결당 CPU·암호화·메모리 오버헤드가 커서 **고밀도 CCU MMO에서는 TCP/KCP보다 노드당 동접 밀도가 낮아질 수 있다.** "MMO=QUIC"로 단정하지 말고 게임 성격으로 고를 것. 빠른 상태 동기는 QUIC라도 신뢰 스트림이 아니라 DATAGRAM으로 보낸다.

### 2.4 백플레인 (`IBackplane`) — Redis 기본, scale-out
동질 노드 + L4 LB(신규 연결 분산, 연결은 수명 동안 노드에 고정). 노드 간 협업은 백플레인으로:
- **세션 디렉터리**: "어느 노드가 어떤 sessionId/userId 보유"를 외부화(Redis).
- **pub/sub 라우팅**: A노드 → B노드의 클라이언트로 메시지 전달.
- **공유 상태**: 룸/매치 등 공유 KV.
- **무중단 join/leave**: SIGTERM → accept 중단 → 진행 세션 드레이닝.
엔진은 토폴로지를 강요하지 않고 인터페이스만 제공, 기본 구현으로 Redis 제공(구매자가 교체 가능).

### 2.5 런타임 데이터 핫스왑 (불변 스냅샷 + atomic 포인터 교체)
설정·게임 데이터 테이블(아이템/룰 등)을 **무중단 실시간 갱신**.
- 패턴: `static std::atomic<std::shared_ptr<const T>> g_data;`
  - **읽기(핫패스)**: `auto snap = g_data.load();` — 락 없음. 진행 중 reader는 옛 스냅샷, 신규는 새 스냅샷 → **torn read 없음.**
  - **쓰기**: 새 스냅샷을 *완성*한 뒤 `g_data.store(new)` / `exchange`. 게시 후 스냅샷은 **불변**.
  - **회수**: `shared_ptr` refcount가 마지막 reader 종료 시 자동 해제. 고경합이면 RCU/hazard pointer/epoch로 최적화.
- **scale-out 연계**: 데이터 버전 갱신을 Redis pub/sub로 브로드캐스트 → 각 노드가 로컬에서 로드 후 스왑.
- 주의: 현 코드가 C++20(이미 `std::format` 사용)이라 `std::atomic<std::shared_ptr>` 사용 가능 — MSVC/Clang 버전 확인, `static` 초기화 순서(명시적 init 또는 Meyers singleton).

### 2.6 엔진 코어 나머지
- **Bootstrap**: 설정 로드 → 서브시스템 초기화 → Run/Stop, 시그널 처리, graceful shutdown.
- **Acceptor**: listen/bind, AcceptEx(또는 epoll accept) 다중 사전 게시, 스로틀링.
- **SessionManager**: 사전할당 풀(샤딩/락프리), ID 발급, **refcount 수명관리**(close ↔ 진행 IO 레이스 차단).
- **IO Engine**: recv 게시·누적, send 큐 + per-session 직렬화 + 백프레셔.
- **Framing**: 길이 프리픽스(헤더 크기/엔디안/최대 크기 설정화).
- **Dispatcher/Worker**: 기본은 워커 큐 분리(앱 로직이 IO 차단 방지), 저지연 옵션(IO 스레드 직접 호출).
- **Logger 재작성**: 비동기·레벨·**stdout 구조화(JSON)** 우선(컨테이너 표준) + 파일/로테이션 옵션.
- **Stats**: 접속 수·처리량·큐 깊이 등 주기 출력 또는 stats 엔드포인트.

### 2.7 공개 SDK 사용 예 (구매자가 만지는 전부)
```cpp
class MyApp : public helix::IServerApp {
  void OnConnect(helix::Session& s) override {}
  void OnMessage(helix::Session& s, helix::ByteSpan msg) override { s.Send(msg); }
  void OnDisconnect(helix::Session& s, helix::CloseReason r) override {}
};
int main() { return helix::Server::FromConfig("config/helix.toml").Run(std::make_unique<MyApp>()); }
```
이 헤더에 `SOCKET`/`OVERLAPPED`/Win32가 **노출되지 않는다**(불투명 핸들만). `include/helix/`(공개) vs `src/`(내부) 분리.

---

## 3. 현재 → 목표 Gap 요약

| 영역 | 현재 | 목표 | 작업량 |
|---|---|---|---|
| Listen/Accept | 없음 | Acceptor 다중 게시 | 큼 (1순위) |
| Session 관리 | 없음 | 풀·ID·refcount | 큼 |
| recv/send | 없음 | IO 엔진 + 송신 큐 | 큼 |
| 프레이밍 | 없음 | 길이 프리픽스(설정화) | 중 |
| 디스패치/워커 | 선언만 | 라우팅 + 잡 큐 | 중 |
| 플랫폼 백엔드 | Win 직접 | `INetBackend` (IOCP/epoll) | 큼 |
| 전송 추상화 | 없음 | `ITransport` (TCP/KCP/QUIC) | 큼 |
| C ABI/다언어 | 없음 | C ABI + C++/C# 래퍼 | 큼 |
| 백플레인/scale-out | 없음 | `IBackplane` + Redis | 큼 |
| 런타임 데이터 핫스왑 | 없음 | atomic 스냅샷 스왑 | 중 |
| 설정 | INI(부분) | TOML + env + 검증 | 중 |
| Logger | 미완성/비안전 | 비동기·레벨·stdout | 중 |
| 배포 | 없음 | Docker(Win+Linux), drain | 중 |
| 빌드/패키징 | VS 전용 | CMake+vcpkg, install/export | 중 |

---

## 4. 단계별 로드맵

추상화 경계(2.1~2.4)가 Phase 0~2에서 서면 이후 단계는 상당 부분 병렬화 가능.

- **Phase 0 — 기반**: CMake+vcpkg 크로스플랫폼 빌드, `include/`vs`src/` 분리, **C ABI 경계 + 추상화 3종(`INetBackend`/`ITransport`/`IBackplane`) 시그니처 확정**, Logger 재작성(stdout 구조화), 기존 결함 정리. *검증: 기존 기능 빌드/기동, 로그 멀티스레드 안전.*
- **Phase 1 — TCP MVP (Windows IOCP)**: Acceptor + Session(refcount) + recv/send + 에코. *검증: 외부 클라이언트 접속·에코 왕복, N 동시접속 안정.*
- **Phase 2 — 메시징 + C ABI + C++ SDK**: 프레이밍, 송신 큐/백프레셔, C ABI export, 공개 핸들러 API, 샘플 에코. *검증: 샘플이 Win32 비노출로 빌드, 단편화 메시지 정확 재조립.*
- **Phase 3 — 설정/컨테이너화**: TOML+env+검증, Docker 이미지(Win+Linux), stdout 로그, health/drain. *검증: 파일/env만으로 재컴파일 없이 설정 변경, 컨테이너 기동.*
- **Phase 4 — Linux 백엔드(epoll)**: `INetBackend` epoll 구현(코어 무변경 목표). *검증: 동일 샘플이 Linux 컨테이너에서 동작.*
- **Phase 5 — 런타임 데이터 핫스왑**: atomic 스냅샷 스왑 + (Redis 연동 시) 버전 브로드캐스트. *검증: 부하 중 데이터 교체에 torn read·크래시 없음.*
- **Phase 6 — Scale-out**: `IBackplane` + Redis(세션 디렉터리/pub-sub/공유상태), L4 LB, graceful join/leave. *검증: 멀티노드 간 크로스노드 메시지, 노드 추가/제거 무중단.*
- **Phase 7 — 멀티 전송**: `ITransport`에 KCP → QUIC(msquic) 추가. *검증: 전송 전환만으로 동일 앱 동작.*
- **Phase 8 — C# SDK**: C ABI 위 P/Invoke 래퍼, C# 샘플, NuGet 패키지. *검증: C#만으로 동일 에코 서버 작성·동작.*
- **Phase 9 — 상품화**: install/export 타깃, SemVer, 문서(퀵스타트·설정/ API 레퍼런스), 샘플 2~3종, 부하/벤치마크.

---

## 5. 핵심 리스크와 사전 결정

- **세션 수명 레이스**: close ↔ 진행 IO 완료. refcount + "pending IO 0 도달 시 반환" 규칙을 Phase 1에서 못 박기. (최대 버그원)
- **C ABI 설계**: 예외 비전파(에러코드), 핸들 수명, 콜백 스레딩/ C# GC 핀닝·델리게이트 수명. 초기에 규약 고정.
- **QUIC 비대칭**: msquic은 자체 I/O 보유 → `INetBackend` 아래가 아니라 별도 전송. 인터페이스가 이 차이를 흡수해야.
- **proactor/reactor 통합**: 백엔드를 proactor로 통일, epoll은 에뮬레이션. Phase 0 인터페이스 실수 시 Linux에서 코어 재작업.
- **백플레인 일관성**: 세션 디렉터리 staleness, pub/sub 전달 보장(at-most-once), 핫키. TTL·재동기 전략 필요.
- **atomic 스냅샷**: `atomic<shared_ptr>` 구현별 내부 락/경합 — 고부하 시 RCU/hazard로 최적화. 스냅샷 게시 후 불변 규칙 엄수.
- **크로스플랫폼 양쪽 = CI/테스트 매트릭스 2배**: Win/Linux 이미지·테스트 모두 자동화 필요.
- **범위 확장 주의**: 단일 Windows TCP → 크로스플랫폼·멀티전송·멀티언어·수평확장으로 커짐. 전부 가능하나 단계가 많음 → MVP(Phase 1~2)로 가치 먼저 증명 후 확장 권장.

---

## 6. 착수 준비 상태
주요 방향이 모두 확정되어 **Phase 0 착수 가능**. Phase 0의 추상화 경계 설계(특히 `INetBackend` proactor 인터페이스 + C ABI 규약)에 설계 시간을 집중하는 것이 이후 전체 일정의 안정성을 좌우한다.
