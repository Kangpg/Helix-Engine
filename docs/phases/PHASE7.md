# Phase 7 — 멀티 전송 (`ITransport`): KCP (QUIC는 후속)

> 기록일: 2026-06-01 · 상태: **KCP 코드 완성(런타임 검증 대기)** · QUIC(msquic)는 7b로 분리
> 전송은 설정으로 선택(`[network] transport = tcp|kcp|quic`). 동일 IServerApp이 전송과 무관하게 동작.

## 목표
세션/프레이밍 위에서 전송을 갈아끼운다. Phase 1~2의 TCP는 INetBackend(IOCP)에
직접 묶여 있었고 `ITransport`는 선언만 있었다 → 이번에 **전송 추상화를 실제 배선**하고
KCP(저지연 신뢰 UDP)를 첫 비-TCP 전송으로 추가했다. 사용자 결정에 따라 **KCP 먼저,
QUIC는 다음**(msquic 의존성 결정 포함).

## 완료한 작업 (KCP)

### KCP 전송 (`ITransport` 구현)
- `ThirdParty/kcp/ikcp.{h,c}` — **사용자 vendoring 필요**: 원본 KCP(skywind3000/kcp, MIT). 재구현하지 않고 임베드.
- `src/transport/kcp/KcpTransport.{h,cpp}` — **신규**. ITransport 구현:
  - 전용 UDP 소켓 + recv 스레드 + update 스레드(자체 I/O — QUIC처럼 INetBackend 밖, backend 인자 무시).
  - **remote 주소(sockaddr 바이트)로 연결 디먹스**. 새 주소 = 새 연결(`OnAccepted`). conv는 첫 패킷에서 `ikcp_getconv`.
  - `ikcp_input`→`ikcp_recv`로 조립한 바이트를 `OnData`. 송신은 `ikcp_send`+`ikcp_flush`, output 콜백이 `sendto`.
  - update 스레드가 10ms마다 `ikcp_update`(전 연결) + idle 타임아웃(기본 30s) 정리→`OnClosed`.
  - `ikcp_nodelay(1,10,2,1)` + `wndsize(256,256)` 저지연 모드. ikcp는 비스레드안전 → 단일 mutex로 직렬화.
- `src/transport/TransportFactory.cpp` — **신규**: `CreateTransport(kind)` (kcp→KcpTransport, tcp→nullptr=코어 기존경로, quic→후속).

### 코어 전송 경로 배선
- `src/core/TransportSession.{h,cpp}` — **신규**: ITransport 기반 공개 `helix::Session` 구현. Send=프레이밍 후 `ITransport::Send`. **ServerCore가 shared_ptr로 소유**(콜백 재진입 Close 시 use-after-free 방지). FrameReader로 수신 디프레이밍.
- `src/core/ServerCore.{h,cpp}` — **수정**: `ITransportSink` 추가 구현(`OnAccepted/OnData/OnClosed`).
  - `_txMode = (transport != Tcp)`. TCP는 기존 Acceptor/Session/IOCP 워커 경로 그대로, KCP/QUIC는 전송이 자체 스레드를 돌리므로 워커 없음.
  - `Start`: 백플레인(scale-out) 연결을 **두 경로 공통**으로 앞당김. txMode면 transport Init/Listen + heartbeat.
  - `Run`(txMode): stop 대기 → `transport->Stop()`(전 연결 OnClosed→OnDisconnect) → 백플레인 정리.
  - `OnAccepted`→TransportSession 생성 + `MakeSessionId(nodeId,seq)`(NodeId 임베드 동일) + 디렉터리 RegisterSession + `OnConnect`. `OnData`→FrameReader→`OnMessage`. `OnClosed`→`OnDisconnect`+Unregister.
  - `Find`/`SessionCount`도 txMode 분기 → **scale-out(크로스노드 라우팅)·Server 공개 API가 KCP에서도 그대로 동작**.

### 설정 / 빌드 / 검증
- `[network] transport = tcp|kcp|quic` (EngineConfig + 검증 + helix.toml + Server.cpp 매핑).
- `CMakeLists.txt`: `LANGUAGES C CXX`(ikcp.c를 C로 컴파일), 전송/세션 소스 + `ThirdParty/kcp/ikcp.c` + include dir, `helix_kcp_echo_client` 샘플.
- `samples/kcp_echo_client/main.cpp` — **신규**: 원본 ikcp로 작성한 C++ 에코 클라(UDP+KCP). N개 프레임 송신→에코 순서·내용 대조→PASS/FAIL.
  - **에코 서버는 기존 `helix_echo_phase1`을 그대로 재사용** — `HELIX_NETWORK__TRANSPORT=kcp`로 띄우면 동일 EchoApp이 KCP 위에서 동작(전송 투명성 입증).

## 설계 결정
- **KCP 먼저, QUIC 후속** — KCP는 의존성 0(ikcp.c)이고 단독 검증 가능. msquic 확보 방식은 7b에서 결정.
- **KCP는 자체 UDP I/O(전송 병렬 배치)** — INetBackend(IOCP)에 UDP를 끼우는 대신 전용 소켓+스레드. 단순·검증 용이. (계획서는 "UDP는 INetBackend 위" 였으나 MVP는 자체 관리; IOCP UDP 통합은 후속 최적화.)
- **전송 투명성** — TransportKind만 바꾸면 같은 IServerApp(에코/라우팅)이 동작. TCP는 기존 고속 경로 무손상 유지.
- **세션 수명은 shared_ptr** — 전송 콜백(타 스레드)에서의 재진입 Close에도 안전.

## 미완료 / 다음 단계
- **QUIC(msquic)** — 7b. msquic 확보(vcpkg vs 사전빌드) 결정 필요. msquic은 자체 I/O라 ITransport에 그대로 흡수.
- ikcp.c 벤더링은 사용자 작업(저장소에 두 파일). 미배치 시 빌드 실패(의도적 — 원본 그대로 쓰기 위함).
- KCP는 현재 단일 mutex 직렬화 → 초고동접 시 샤딩/연결별 락 최적화 후속.
- C ABI/C#에 전송 선택 노출은 후속(설정으로는 이미 가능).

## 검증 상태
**코드 완성 / 런타임 검증 대기.** 절차(Windows):
1. ikcp.c/ikcp.h 를 `ThirdParty/kcp/` 에 배치.
2. 빌드: `cmake --preset windows-x64` → `cmake --build --preset windows-debug`.
3. KCP 에코 서버: `$env:HELIX_NETWORK__TRANSPORT="kcp"; .\build\windows-x64\Debug\helix_echo_phase1.exe`
   → 로그 `transport=kcp` + `kcp transport listening on ...:/udp`.
4. 클라: `.\build\windows-x64\Debug\helix_kcp_echo_client.exe 127.0.0.1 5000 200 128` → `RESULT : PASS`.
5. (옵션) cluster=on + KCP 동시: KCP 세션도 디렉터리 등록·크로스노드 라우팅 동일 동작.

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md (2.3 전송 추상화)
- 이전 단계: ./PHASE6.md
