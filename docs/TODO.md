# helix-engine — 남은 작업 (TODO / 로드맵)

> 기록일: 2026-06-24 · 출처: 세션 설계 논의 + [PHASE4.md](phases/PHASE4.md) 후속.
> 계획 본문은 [HELIX_ENGINE_PLAN.md](../HELIX_ENGINE_PLAN.md), 현재 사용·지원 범위는 [README](../README.md)를 따른다.

## 범례
- ✅ **Linux 샌드박스에서 빌드·검증 가능** (바로 진행)
- ⚠️ **Windows / 외부 의존** — 코드는 가능하나 검증은 해당 환경에서 직접
- 🧩 **앱/확장** — 설계 + 구현 (엔진 코어는 무수정, 팩토리/주입으로)

---

## 1. 코어 — 미완 전송·백엔드

### 1.1 RIO 데이터 경로 (Windows) ⚠️  *(task #3)*
- `WindowsIocpBackend`에 RIO 등록버퍼 풀 + 완료 큐 데이터 경로 구현. `Auto`/`IocpRio`
  선택 시 활성, 미지원 환경은 일반 IOCP로 폴백(`Name()`이 실제 모드 보고). 현재 stub.
- **왜**: 초고밀도·초고패킷레이트(FPS/MOBA) 워크로드에서 syscall/OVERLAPPED 오버헤드 절감.
- **검증**: MSVC x64 빌드 + `echo_bot`로 IOCP 대비 packet-rate 비교. *(Linux 샌드박스 불가)*
- **선행 판단**: 벤치에서 IOCP가 packet-rate 한계에 부딪힐 때 착수(YAGNI). 설계는 이미
  열려 있어 코어 수정 없이 추가 가능.

### 1.2 QUIC 전송 (ITransport) ⚠️  *(task #4)*
- `QuicTransport` = msquic 래핑, `CreateTransport` 팩토리 등록. 코어 무수정(개방/폐쇄).
- **왜**: 모바일 망 전환(connection migration), TLS1.3 내장, 스트림 멀티플렉싱.
- **검증**: msquic 의존 추가 + Windows/Linux 빌드. *(현재 샌드박스 불가)*

---

## 2. C ABI · 배포 견고화

### 2.1 호출 규약 명시 — `HELIX_CALL` ✅(헤더 변경, 검증 쉬움)
- `helix_c.h`의 모든 export 함수에 `HELIX_CALL`(Windows=`__cdecl`, 그 외 공백) 부여.
- **왜**: x64는 호출 규약이 단일이라 무영향이지만, x86(32-bit) 지원 시 C# P/Invoke 기본
  (stdcall)과 C 기본(cdecl) 불일치로 스택 손상. 미리 못박아 두면 안전. (ABI 가이드 권장)

### 2.2 ABI 호환성 문서 — `docs/ABI.md` ✅
- 매트릭스 명시: **C ABI(helix.dll)=컴파일러·언어 독립 안정 경로 / C++ SDK(정적 lib)=
  동일 컴파일러·stdlib·`_GLIBCXX_USE_CXX11_ABI` 필요 / 기준 x64**. 소비자 가이드.
- **왜**: C++ SDK는 std 타입을 인터페이스에 노출 → ABI 글의 제약이 그대로 적용됨.

### 2.3 Windows 컨테이너 다듬기 ⚠️
- `Dockerfile.windows`를 nanoserver + `/MT`(정적 CRT)로 슬림화 + `SetConsoleCtrlHandler`
  (`CTRL_SHUTDOWN_EVENT`→`Server::Stop()`)로 graceful 종료.
- **왜**: 현재 servercore 런타임 패키징 + Windows 컨테이너는 `docker stop`이 SIGTERM이
  아니라 셧다운 이벤트라 샘플의 `signal(SIGTERM)`이 안 잡힘.

---

## 3. 로그 / 이벤트 파이프라인 (Kafka) 🧩

### 3.1 `ISink` 2등급 인터페이스 (lossy / durable)
- 비동기 **바운드 큐 + 백그라운드 플러시**(librdkafka), 팩토리/주입(코어 무수정).
  - **lossy**(운영 로그): 큐 가득 차면 drop/샘플 — 핫패스 절대 무블로킹.
  - **durable**(원장 이벤트: 게임결과·재화): `acks=all`·RF≥3, 막히면 backpressure/디스크
    버퍼(절대 drop 금지).
- **왜**: 로그·이벤트를 Kafka로 fire-and-forget. 두 등급을 섞으면 안 됨(앞 논의).

### 3.2 레퍼런스 컨슈머 샘플 (`samples/`)
- Kafka `game-results` → **Redis ZSET**(ZADD/ZINCRBY, 매치ID 멱등) = CQRS 리더보드 쓰기.
  읽기는 게임서버가 Redis ZREVRANK/ZREVRANGE 직접. "엔진은 produce만, 소비는 분리" 예시.

---

## 4. 룸 / 멀티노드 모델 🧩

### 4.1 per-thread reactor 옵션 (share-nothing 룸)
- 스레드마다 `EpollBackend` 인스턴스 + 연결→소유 룸 스레드 라우팅. 룸 상태 락-프리.
- **부하 기반 배치**(룸 "개수" 균등 → 예상 "부하" 균등)로 핫스팟 방지. 룸 스레드 블로킹
  금지(DB는 비동기). 현재 엔진은 공유 epoll + N워커(메일박스형) — 이건 옵션 추가.

### 4.2 게이트웨이 + 핸드오프 + 크로스서버 배틀 설계 (`docs/`)
- 게이트웨이(연결 고정·라우팅), 글로벌 디렉터리(player→노드), 핸드오프 프로토콜
  (quiesce→직렬화→전송→복원, 이중소유 금지), 크로스서버 **전용 배틀 인스턴스**(여러 홈
  서버 플레이어 소환). 전부 Redis 백플레인(디렉터리·pub/sub·RouteToSession) 위에.
- **데이터/컨트롤 플레인 분리** 원칙 명문화: 라우팅 결정은 컨트롤(드묾), 패킷은 직통.
- **채널 매핑**: 채널=노드 / 채널=노드 내 서브 / 채널=노드 분산 — 앱이 선택, 엔진은
  SessionId(nodeId 임베드)·디렉터리·라우팅만 제공.

---

## 5. 최적화 / 정리

- **5.1** epoll `RearmLocked` 최적화 — 현재 매 fd 처리 끝마다 `epoll_ctl(MOD)`. "상태
  변경 시에만 재무장"으로 syscall 절감.
- **5.2** echo_phase1 `Snapshot`(`std::atomic<std::shared_ptr>`)은 **GCC 12+** 필요 —
  README/문서 명시(또는 GCC<12 폴백 경로).
- **5.3** 벤치마크 CI 활성화 — `tools/benchmark.py` 매트릭스(tcp/kcp) → 리포트 아티팩트.
- **5.4** RESP/KCP의 Linux 경로 통합테스트 추가(현재 단위/에코 검증 + 풀 링크까지).

---

## 우선순위 제안
1. **지금 바로(검증 쉬움):** 2.1 `HELIX_CALL` + 2.2 `ABI.md`.
2. **실효 큰 확장:** 3.1 `ISink` + 3.2 컨슈머 샘플 (Kafka 로깅·랭킹).
3. **프로젝트 핵심 설계:** 4.1 per-thread 룸 + 4.2 게이트웨이/핸드오프 문서.
4. **Windows 환경에서:** 1.1 RIO, 1.2 QUIC, 2.3 Windows 컨테이너.

## 완료됨 (참고)
Phase 0~9 + Phase 4(Linux epoll·풀 CMake 빌드·Docker·CI). 상세 [phases/](phases/).
