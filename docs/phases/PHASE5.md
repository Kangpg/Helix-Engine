# Phase 5 — 런타임 데이터 핫스왑 + graceful drain

> 기록일: 2026-06-01 · 상태: 코드 완성(빌드 검증됨, 런타임 검증 대기)
> 참고: Phase 4(Linux epoll)는 의도적으로 보류 — 검증 환경(Windows) 우선.

## 목표
설정/데이터를 무중단으로 실시간 교체(atomic 스냅샷)하고, 종료 시 진행 중 세션을 정리하는 graceful drain을 넣는다. 둘 다 Windows에서 검증 가능. (계획서 Phase 5 + Phase 4의 drain 항목)

## 완료한 작업
- `include/helix/runtime_data.h` — **신규(공개 승격)**: `helix::Snapshot<T>`(`Load/Store/Exchange`, `std::atomic<std::shared_ptr<const T>>`), `helix::Versioned<T>`. 앱이 직접 사용. `helix.h`에 포함.
- `src/data/RuntimeData.h` — **수정**: 공개 헤더로 이전, 이제 `include/helix/runtime_data.h`를 include하는 shim.
- `src/core/ServerCore.{h,cpp}` — **수정**: graceful drain.
  - `Stop()`: 신호만 — `_stopRequested` 표시 + accept 중단 + `CloseAll` + 워커 Wake + cv notify. (시그널 핸들러/타 스레드에서 호출 안전)
  - `Run()`: stop 요청 대기(cv) → **드레이닝**(SessionCount 0 될 때까지 대기, `drainTimeoutMs` 한계) → `_running=false` → 워커 join → 정리(backend shutdown, WSACleanup). 드레이닝 중 워커는 close 완료를 계속 배수.
  - 이전의 이중 join/즉시종료 문제 해소. 타임아웃 초과 시 경고 후 강제 종료.
- 설정: `EngineConfig.drainTimeoutMs` + `[shutdown] drain_timeout_ms`(EngineConfig.cpp 로드, Server.cpp 매핑, `config/helix.toml`).
- `samples/echo_phase1/main.cpp` — **수정**: 핫스왑 데모. 백그라운드 스레드가 2초마다 배너 스냅샷을 `Store`, 핸들러(워커 스레드)가 `Load`로 락 없이 읽음. 에코 프로토콜은 불변(봇 통과 유지). Ctrl-C → graceful 종료.

## 설계 결정
- **Snapshot을 공개 SDK로** — 데이터 핫스왑은 앱의 관심사이므로 `helix::Snapshot<T>`를 구매자가 직접 사용. 엔진 개입 불필요(클러스터 브로드캐스트만 Phase 6에서 연결).
- **Stop=신호 / Run=드레이닝·정리 분리** — 시그널 안전성 + 단일 정리 경로. drain 타임아웃으로 무한 대기 방지.
- **불변 스냅샷 규칙** — 게시 후 수정 금지. reader는 Load한 shared_ptr 수명 동안 안전.

## 미완료 / 다음 단계로 이월
- Phase 4(Linux epoll + Docker Linux 이미지) — 보류 중.
- 데이터 버전 클러스터 브로드캐스트(`IBackplane.PublishDataVersion` → 각 노드 Store) — Phase 6.
- drain 중 신규 데이터/통계 노출, health 엔드포인트 — 후속.
- 비동기 로거 — 후속.

## 검증 상태
**빌드 검증됨 / 런타임 검증 대기.** Windows/MSVC. 절차:
- 빌드 후 서버 실행 → `python tools/echo_bot.py churn --port 5000 --clients 500 --rounds 20`.
  - 서버가 2초마다 배너를 교체하는 동안 churn이 동시에 connect/read → **PASS면 동시 스냅샷 Load/Store 안전**(torn read·크래시 없음).
- Ctrl-C → 로그 `stop requested (draining) ...` → `stopped` 순서, 크래시 없이 종료(graceful drain).
- `HELIX_SHUTDOWN__DRAIN_TIMEOUT_MS=1000` 등으로 타임아웃 동작 확인.

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md
- 이전 단계: ./PHASE3.md (Phase 4는 보류)
