# helix-engine — 고성능 서버 구조 실현 가능성 분석 보고서

> 주제: 이런 구조의 서버를 helix로 만들 수 있는가 —
> ① 네트워크: **IOCP + RIO 하이브리드**, ② **스레드 × N 룸 락프리(share-nothing)**, ③ 기타.
> 작성일: 2026-06-24 · 근거: 현재 코드(`src/`, `include/`) 직접 분석.

---

## 0. 요약 (Executive Summary)

**결론: 둘 다 현재 아키텍처 위에서 만들 수 있다. 그것도 공개 SDK·C ABI 무수정으로.**

이유는 helix의 골격 자체가 이 목적을 위해 설계됐기 때문이다:
- **3개 내부 추상화**(`INetBackend`/`ITransport`/`IBackplane`) + **proactor로 통일된 I/O 계약**.
- 새 구현·새 러너 "추가"로 확장(개방/폐쇄·DIP) — 코어 로직은 인터페이스에만 의존.

| 목표 | 현재 상태 | 남은 작업 | 난이도 | 코어 수정 |
|---|---|---|---|---|
| IOCP+RIO 하이브리드 | 추상/enum/폴백 **설계 완료**, RIO 데이터경로 stub | RIO 등록버퍼·완료큐 구현 | 중상 | **불필요** |
| 스레드×N 룸 락프리 | 공유 I/O + N워커(메일박스형) | per-thread reactor(샤드) 러너 추가 | 중 | 러너만 |
| 메모리 풀·코어 피닝·제로카피 | 일부 토대(스냅샷·백프레셔) | 풀/피닝/RIO버퍼 | 중 | 부분 |

---

## 1. IOCP + RIO 하이브리드 네트워크

### 1.1 무엇이고 왜
- **IOCP**: Windows 표준 proactor. 범용·안정·고밀도 연결. 단 작업마다 `OVERLAPPED` 할당 +
  커널 전환 비용.
- **RIO(Registered I/O, Win8/Server2012+)**: 버퍼·요청큐·완료큐를 **사전 등록**하고 폴링으로
  완료를 거둬, syscall·할당·복사를 거의 없앤 고속 데이터 경로.
- **하이브리드**: 수락/연결·범용 경로는 IOCP, **데이터(recv/send) 핫패스만 RIO**. 초고
  패킷레이트(FPS/MOBA/대규모 동접)에서 IOCP 한계를 RIO로 넘긴다.

### 1.2 helix 현재 상태 (코드 근거)
- `src/net/INetBackend.h` — **proactor로 통일된 단일 계약**: `Post{Accept,Recv,Send}()` 로
  걸고 `Poll()` 이 "완료"를 회수. IOCP·RIO·epoll이 전부 이 계약 뒤에 숨는다.
- 같은 파일의 `enum class IoBackendKind { Auto, Iocp, IocpRio, Epoll }` — **RIO 선택지가
  이미 1급 시민**. `BackendConfig.kind` 로 설정(`config/helix.toml`의 `backend=iocp_rio`).
- `src/net/WindowsIocpBackend.cpp` — `IocpRio`/`Auto` 요청을 받고 RIO 가용성을 탐지하는
  자리(`_rioRequested`)는 있으나, **데이터 경로는 stub**: `_rioActive = false; // TODO(Phase 4)`.
  현재는 일반 IOCP(`AcceptEx`+`WSARecv`+`WSASend`)로 폴백하고 `Name()`이 실제 모드 보고.
- 즉 **"하이브리드 골격"은 이미 코드에 박혀 있고, RIO 데이터 경로만 비어 있다.**

### 1.3 하이브리드의 구체 매핑
한 백엔드(`WindowsIocpBackend`, IocpRio 모드) 안에서:
- **수락 경로**: 그대로 IOCP + `AcceptEx`(범용·검증).
- **데이터 경로**: 소켓 등록 시 `RIO` 핸들로 전환 → **RIO 등록버퍼 풀**에서 recv/send 버퍼
  배정 → `RIOReceive`/`RIOSend` 로 게시 → **RIO 완료큐**를 `RIONotify`+IOCP로 깨워
  `Poll()`에서 거둠. → `INetBackend`의 `Post*`/`Poll` 계약을 **그대로** 만족.

### 1.4 갭과 구현 작업 (코어 무수정)
1. RIO 함수 포인터 로드(`WSAID_MULTIPLE_RIO` via `WSAIoctl`).
2. **등록버퍼 풀**(`RIORegisterBuffer`) — recv/send 버퍼를 사전 등록, 수명 관리(여기가 핵심·난점).
3. 소켓당 **RIORequestQueue**(`RIOCreateRequestQueue`) + 공용 **완료큐**(`RIOCreateCompletionQueue`,
   IOCP에 연결).
4. `PostRecv/PostSend` → `RIOReceive/RIOSend`(등록버퍼 slice 지정), `Poll` → `RIODequeueCompletion`
   + `RIONotify` 재무장.
5. RIO 미지원/할당 실패 시 일반 IOCP로 폴백(`_rioActive=false`).
- **계약 일치**: `IoContext`(버퍼/길이)·완료 통지·송신 직렬화 의미는 그대로 → `ServerCore`/
  `Session`/`Acceptor` **전혀 안 건드림**. (이미 epoll을 같은 방식으로 무수정 추가했음 = 검증된 패턴)
- **난이도**: 중상. 어려운 건 RIO 로직이 아니라 **등록버퍼 수명/소유권**(연결 churn에서 누수·
  중복등록 방지).

### 1.5 기대 효과 / 한계
- syscall·`OVERLAPPED` 할당 제거 + 사전등록 버퍼(사실상 **제로카피 경로**) → 초고패킷레이트에서
  처리량·지연·CPU 모두 이득.
- **한계**: Win8/Server2012+ 전용, 버퍼 사전등록 메모리 고정, 관리 복잡도. 그래서 **기본은 IOCP,
  RIO는 옵트인 + 폴백**이 옳다(이미 그 설계). 착수 기준: echo_bot 벤치에서 IOCP가 packet-rate
  한계에 부딪힐 때(YAGNI).

---

## 2. 스레드 × N 룸 락프리 구조

### 2.1 모델
- **share-nothing / shard-per-thread / 액터**: 룸 1개를 워커 1개가 **전담**. 그 룸 상태는 단일
  스레드만 만지므로 **락이 원천적으로 불필요**(single-writer). Redis(샤드당 싱글스레드)·
  Seastar/ScyllaDB(코어당 샤드)와 같은 계열.

### 2.2 helix 현재 상태 (코드 근거)
- `src/core/ServerCore.cpp::WorkerLoop` — **공유 `INetBackend` 1개를 N 워커가 동시에 `Poll`**.
  즉 아무 워커나 아무 연결의 완료를 처리(워크-셰어). 룸 친화(affinity)는 없음.
- `src/session/Session.cpp` — 송신은 `_sendLock`(per-session mutex)으로 직렬화. recv는 단일
  outstanding이라 순서 보장. **룸 개념은 엔진에 없고 앱(IServerApp) 계층.**
- 정리: 현재는 **"공유 I/O 풀 + 락 보호 세션"** = 멀티노드/범용엔 좋지만 "스레드당 룸 락프리"는
  아직 아니다(단, 그 위에 얹을 수 있는 구조).

### 2.3 락프리 룸으로 가는 길 — per-thread reactor(샤드)
- **각 워커 스레드가 자기 `INetBackend` 인스턴스(EpollBackend/IocpRio) + 자기 연결 + 자기 룸을
  소유.** 그 연결의 모든 I/O·로직이 같은 스레드에서 → **락 0 + 스레드 간 홉 0**(완전 share-nothing).
- 이 모델에선 `Session::_sendLock`도 **제거 가능**(단일 스레드 소유 → 직렬화가 공짜).
- 대안: 공유 I/O 유지 + **룸 메일박스(SPSC/MPSC 큐)** 로 한 홉 — 기존 helix에 더 쉽게 얹힘.

### 2.4 갭과 구현 (러너만 변경, 인터페이스 유지)
1. `ServerCore`가 **N개 샤드**(각: `INetBackend` + `Acceptor` + `SessionManager`)를 띄움.
2. **수락 분산**: Linux `SO_REUSEPORT`(리스너 N개 → 커널이 연결 분배) / Windows 리스너별
   `AcceptEx`. 또는 단일 수락 후 대상 룸 스레드로 핸드오프.
3. **연결→룸→스레드 고정**: 앱이 "이 연결은 룸 R, R은 스레드 T" 를 정함(현재 SessionId의 nodeId
   임베드처럼, roomId/threadId를 얹음).
4. **부하 기반 배치**(룸 "개수" 균등 → 예상 "부하" 균등). 룸 스레드 **블로킹 금지**(DB는 비동기 풀).
5. `INetBackend`/`Session`/`FrameCodec` **계약은 그대로** — `EpollBackend`는 이미 다수 워커 동시
   `Poll`을 지원하지만, 샤드 모델에선 인스턴스를 스레드당 1개로 두면 그 자체가 락프리.
- **난이도**: 중. 손대는 곳은 **코어 러너(`ServerCore`)와 배치 정책**이지, 백엔드/세션/프레이밍
  계약이 아니다. (Linux에서 먼저 구현·검증 가능 — 샌드박스 OK)

### 2.5 주의점
- **크로스룸 상호작용**(전체 채팅·파티) → 상대 룸 메일박스 메시지패싱 or Redis. 룸이 독립적일수록 유리.
- **마이그레이션/리밸런스**: 룸을 다른 스레드로 옮기려면 quiesce→소유권 이전→재개(비쌈). 룸이
  단명이면 안 옮기고 다음 배치로 균형.
- **블로킹 금지**: 룸 스레드의 동기 DB/네트워크 호출은 그 스레드의 모든 룸을 멈춤.

---

## 3. 기타 고성능 요소 (등등)

| 요소 | helix 현재 | 도달 방법 |
|---|---|---|
| 제로카피 송신 | recv→frame→send 복사 존재 | RIO 등록버퍼 = 사실상 제로카피 / scatter-gather(WSABUF 다중) |
| 메모리 풀·슬랩 | `new/delete`(Session·버퍼) | 세션·버퍼 사전할당 풀(계획서 Phase4 보강 항목) |
| 완료 배치 | **이미 있음** — `Poll(out, maxOut, …)` 배치 회수 | 송신 코얼레싱 추가 |
| 코어 피닝/NUMA | 없음 | per-thread reactor와 결합 — 스레드를 코어에 핀 |
| 런타임 데이터 핫스왑 | **이미 있음** — `Snapshot<T>`(불변+atomic 교체, 락프리 읽기) | 그대로 활용 |
| 백프레셔 | **이미 있음** — `maxSendQueue`/`BackpressureFull` | 정책 튜닝 |
| 무락 자료구조 | 세션맵은 mutex | 샤드화로 맵 자체를 스레드 로컬화 → 락 제거 |

---

## 4. 종합 평가

- **"이런 구조의 서버 = 만들 수 있다."** helix의 3-추상화 + proactor 통일 계약이 정확히 이걸
  위한 골격이고, IOCP+RIO·per-thread 룸은 모두 **"새 구현/새 러너 추가"** 로 들어간다.
- 공개 SDK(`IServerApp`/`Server`)와 C ABI는 **그대로** — 구매자 코드는 영향 0.
- 이미 **epoll 백엔드를 코어 무수정으로 추가**해 동일 패턴을 검증했다(Phase 4). RIO도 같은 자리에,
  per-thread 룸은 러너 레벨에 들어간다.

---

## 5. 권장 로드맵 (실현 순서)

1. **per-thread reactor(샤드) 옵션** — 락프리 룸의 토대. **Linux에서 먼저 구현·검증 가능**.
   `ServerCore`에 샤드 러너 + `SO_REUSEPORT` 수락분산 + 룸→스레드 라우팅.
2. **RIO 데이터 경로** — IOCP+RIO 하이브리드 완성(Windows). 등록버퍼 풀이 핵심. (벤치로 필요성
   확인 후 착수)
3. **메모리 풀 + 코어 피닝** — 샤드와 결합해 할당·캐시·NUMA 이득 극대화.
4. **제로카피/코얼레싱** — RIO 등록버퍼 + scatter-gather 송신.

각 단계는 `INetBackend`/`ITransport`/`IBackplane`/C ABI 계약을 깨지 않는다 → 점진 도입 가능.

---

## 6. 결론
helix는 "고성능 게임 서버 코어"를 **구매자가 저수준을 몰라도** 쓰도록 추상화한 엔진이고, 그
추상화 경계가 바로 **IOCP+RIO 하이브리드와 스레드×N 락프리 룸을 코어 수정 없이 끼우기 위한
설계**다. 따라서 질문의 구조는 **신규 구조가 아니라 이미 열려 있는 확장점**이며, 남은 건 RIO
데이터 경로(Windows)와 per-thread reactor 러너(우선 Linux 검증)의 **구현**뿐이다.

> 관련: 작업 항목은 [TODO.md](TODO.md)(1.1 RIO, 4.1 per-thread 룸), 계획은 [HELIX_ENGINE_PLAN.md](../HELIX_ENGINE_PLAN.md).
