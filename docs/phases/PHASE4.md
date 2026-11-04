# Phase 4 — Linux 백엔드 (epoll) · 크로스플랫폼 TCP 코어

> 기록일: 2026-06-24 · 상태: **완료(검증됨)**. Windows IOCP 경로는 무변경.
> 계획서의 확정 방향("Windows + Linux 둘 다, `INetBackend`로 IOCP/RIO + epoll") 중
> Linux epoll 경로를 구현하고, 코어 TCP 경로를 크로스플랫폼화했다.

## 목표
엔진의 TCP 경로를 Linux(컨테이너)에서 빌드·동작시킨다. 코어(`ServerCore`/`Acceptor`/
`Session`)는 수정 최소화 + 플랫폼 차이는 단일 셰임으로 흡수(SRP). 백엔드는 새 구현
추가만으로(개방/폐쇄·DIP) 코어 무수정.

## 완료한 작업

### epoll 백엔드 (reactor→proactor 에뮬레이션)
- `src/net/EpollBackend.{h,cpp}` — **신규**: `INetBackend` 구현. epoll은 준비(reactor)
  통지지만 코어는 완료(proactor)를 전제하므로, `Post{Accept,Recv,Send}()`는 fd에
  관심(EPOLLIN/EPOLLOUT)을 **EPOLLONESHOT**으로 무장만 하고, `Poll()`이 그 자리에서
  `accept4()/recv()/send()`를 수행해 "완료"로 포장해 돌려준다.
- 계약 일치:
  - **Recv**: 1 Post = 1 recv = 1 완료. `recv()==0` → `bytes=0`(상위가 PeerClosed).
  - **Send**: `WSASend`처럼 **버퍼 전체 송신 후** 완료. 부분 송신이면 진행분
    (`sentOffset`)을 기록하고 EPOLLOUT 재무장(완료 미발생) → 다 보내야 완료.
  - **Accept**: `accept4()` 결과를 `ctx->acceptSocket`에 채워 완료.
  - **Wake()**: `eventfd(EFD_SEMAPHORE)`로 `ctx==nullptr` 완료 1건(종료/깨우기).
- **스레드 안전**: 다수 워커가 `Poll()` 동시 호출. EPOLLONESHOT으로 한 fd의 준비는
  정확히 한 워커에만 배달. 레지스트리/무장 변경만 `_mtx`로 보호(syscall은 락 밖).
- **핵심 버그 수정(테스트로 발견)**: EPOLLONESHOT은 이벤트 1건이 뜨면 그 fd의 **모든**
  관심을 무장 해제한다. 따라서 `EPOLLOUT`만 떴는데 `EPOLLIN`(recv)이 아직 대기 중이면
  recv가 고아가 되어 영영 안 뜬다. `Poll()`은 매 fd 처리 끝에 **남은 pending을 다시
  무장**(`RearmLocked`)해 이 레이스를 막는다.
- fd 상태(`_fds`)는 fd 번호로 키잉 → OS의 fd 재사용으로 맵 크기가 최대 동시 fd로
  유계(churn에도 무한 증가 없음).

### 플랫폼 소켓 셰임
- `src/net/PlatformNet.h` — **신규**: Winsock 스타일 호출부(`SOCKET`/`closesocket`/
  `WSASocketW`/`WSAStartup`/`WSAGetLastError`/`SOCKET_ERROR` …)가 POSIX에서 그대로
  컴파일되도록 하는 **단일 지점**(SRP). `#ifdef _WIN32`를 코드 전반에 흩뿌리지 않는다.
  공개 헤더(`include/helix/`)에는 절대 포함하지 않음(저수준 비노출 규칙).

### 코어 크로스플랫폼화
- `src/net/Acceptor.cpp` — Winsock 직접 include 제거(→ 셰임). `SO_UPDATE_ACCEPT_CONTEXT`/
  AcceptEx 사전소켓 생성은 `#ifdef _WIN32`. epoll에선 `accept4`가 수락 시점에 소켓을
  만들므로 사전 생성 불필요, `concurrentAccepts`는 1로 수렴(준비 단위). `SO_REUSEADDR`
  추가, `getpeername` 길이를 `socklen_t`로.
- `src/session/Session.cpp`, `src/core/ServerCore.cpp` — Winsock include → 셰임.
  `WSAStartup/Cleanup`은 POSIX에서 no-op.
- `src/net/NetBackendFactory.cpp` — Linux에서 `EpollBackend` 생성 연결.

### enum 명칭 통일(정본화)
- `include/helix/types.h`의 protobuf 스타일 명칭(`TransportType`/`SessionCloseReason`)을
  정본으로, 잔여 코드의 옛 명칭(`TransportKind`/`CloseReason`)을 일괄 전파.
  대상: `Session.*`, `SessionManager.cpp`, `Acceptor`, `helix_c.cpp`, `TransportSession.*`,
  `ITransport.h`, `KcpTransport.*`, `TransportFactory.cpp`, `server_app.h`, `session.h`.

## 검증 상태 (Linux, g++ 11.4, C++20)
- `tests/epoll_backend_test.cpp` — **신규**: `INetBackend` 계약을 실제 루프백 소켓으로
  직접 검증(Register/PostAccept/PostRecv/PostSend/Poll/Wake, peer-close, partial-send).
  **18/18 PASS.**
- `tests/echo_e2e_test.cpp` — **신규**: `EpollBackend + Acceptor + SessionManager +
  Session + FrameCodec`를 `ServerCore::WorkerLoop`와 동일 디스패치로 엮은 에코 서버
  end-to-end. 3프레임 echo / 200KB(부분 송신 경로) / 20동시 클라이언트 /
  OnConnect·OnMessage 카운트. **ALL PASS.**
- 빌드 예:
  ```bash
  g++ -std=c++20 -O2 -pthread -I include -I src \
    src/net/EpollBackend.cpp src/net/Acceptor.cpp \
    src/session/Session.cpp src/session/SessionManager.cpp \
    tests/echo_e2e_test.cpp -o echo_e2e && ./echo_e2e
  ```

## 풀 Linux 빌드 완성 (후속, 2026-06-24)
- `cluster/RespClient.cpp`, `transport/kcp/KcpTransport.cpp` 를 PlatformNet 으로 통합
  (마지막 Winsock 잔재 제거). PlatformNet 에 `netdb.h`/`sys/time.h` 추가(getaddrinfo,
  timeval). KCP recv 루프는 `SO_RCVTIMEO` 로 종료(Linux 는 close 가 블로킹 recvfrom 을
  안정적으로 못 깨우므로).
- 잔여 enum 정리: `BackplaneKind` 값을 PascalCase(`None`/`Redis`)로.
- `CMakeLists.txt` 크로스플랫폼화: 비-Windows skip 제거, 백엔드 소스 플랫폼 선택
  (`WindowsIocpBackend`↔`EpollBackend`), 링크 분기(`ws2_32/mswsock`↔`Threads::Threads`).
  Windows 전용 테스트 클라이언트(`helix_kcp_echo_client`)는 `if(WIN32)` 가드.
- 샘플 SIGTERM 핸들링(echo/chat) — 컨테이너 `docker stop` → graceful drain.
- `Dockerfile.linux` — 멀티스테이지(debian:bookworm), 비루트, tini, stdout 로그.
- `.github/workflows/linux-ci.yml` — epoll 테스트 + 풀 CMake 빌드 + 도커 이미지 잡.

### 검증 (Linux, CMake 4.x, g++/gcc 11~13)
- `cmake -S . -B build && cmake --build build` → `libhelix.a`, `libhelix_capi.so`,
  `helix_chat_room`, `helix_route_phase6` 빌드. `cmake --install` 로 `find_package(helix)`
  소비 산출물(헤더·라이브러리·helixConfig.cmake) 생성 확인.
- `helix_chat_room` 실서버 기동: `backend=epoll` 로 리스닝, 2 클라이언트 브로드캐스트
  end-to-end **PASS**, SIGTERM 으로 graceful 종료.
- 모든 `src/*.cpp` 가 Linux 에서 컴파일(유일 예외 `WindowsIocpBackend.cpp` = Windows 전용,
  CMake 가 비-Windows 에서 제외). `Snapshot` 핫스왑 샘플(echo_phase1)은 `std::atomic<
  std::shared_ptr>` 때문에 **GCC 12+** 필요(MSVC/Clang/bookworm 이상 OK).

## 알려진 한계 / 후속
- `concurrentAccepts`는 epoll에서 1로 수렴(IOCP의 N개 사전 게시와 의미가 다름).
- 매 fd 처리 끝의 `RearmLocked`는 `epoll_ctl(MOD)` 1회 추가 — 정확성 우선. 추후
  "변경 시에만 재무장"으로 최적화 가능.
- RIO(Windows 고속 경로)·QUIC은 별도 단계(둘 다 Windows/외부 의존이라 Linux 샌드박스
  검증 불가).

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md
- 백엔드 인터페이스: ../../src/net/INetBackend.h
