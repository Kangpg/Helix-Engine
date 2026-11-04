# Phase 3 — 설정(파일+환경변수) + 로거 + 컨테이너화

> 기록일: 2026-06-01 · 상태: 부분 완료 (설정/로거 완성, Docker는 스캐폴드, 빌드·실행 미검증)

## 목표
모든 설정을 코드 밖으로(파일 + 환경변수), 기동 시 검증, 컨테이너 친화 로깅(stdout), Docker 패키징. 재컴파일 없이 포트·스레드·백엔드·프레이밍·로그레벨 변경. (계획서 Phase 3)

## 완료한 작업
- `src/config/EngineConfig.{h,cpp}` — **신규**: `EngineConfig`(network/buffers/framing/logging 필드) + `ConfigLoader`.
  - 레이어링(12-factor): 코드 기본값 → 파일 → **환경변수 오버라이드**(`HELIX_<SECTION>__<KEY>`).
  - 파서: TOML/INI 공통 부분집합(`[section]` + `key = value`, `#`/`;` 주석, 따옴표 제거). 파일 없으면 기본값으로 성공.
  - **기동 시 검증**: backend/header_size/level/sink 등 화이트리스트, 명확한 에러 문자열. 실패 시 `Load`가 nullopt.
  - 추후 toml++ 등으로 파서 백엔드만 교체 가능(스키마 동일).
- `src/log/Log.{h,cpp}` — **신규**: 레벨(Debug/Info/Warn/Error)·스레드안전(mutex)·타임스탬프·**stdout 우선**(+옵션 파일). `HLOG_*` 매크로(레벨 선검사). 컨테이너 로그 수집 표준에 맞춤. (비동기 큐는 후속)
- `src/core/Server.cpp` — **수정**: `FromConfig`가 `ConfigLoader`로 로드+검증→로거 초기화→`EngineConfig`→`ServerCoreConfig` 매핑(backend 문자열→`IoBackendKind` 포함). 설정 오류 시 `Run`이 `ConfigError`로 거부.
- `src/core/ServerCore.cpp` — **수정**: 기동/종료 로그(`HLOG_INFO` listening/stopping, 백엔드명·스레드수).
- `config/helix.toml` — **신규**: 외부화된 기본 설정 + 환경변수 오버라이드 안내.
- `Dockerfile.windows` + `.dockerignore` — **신규(스캐폴드)**: 런타임 패키징(빌드 산출물 + config 복사, stdout 로그, 환경변수 주입). Windows 컨테이너.
- `CMakeLists.txt` — **수정**: `EngineConfig.cpp`, `Log.cpp`, `helix_c.cpp` 추가.

## 설계 결정
- **12-factor 설정** — 파일은 기본값/구조, 배포별 차이는 환경변수. 컨테이너에 최적.
- **기동 시 검증 + 명확 에러** — 잘못된 설정으로 조용히 뜨지 않음(라이브 서버는 검증 약함).
- **stdout 로깅 우선** — 컨테이너는 파일 대신 stdout. sink=both로 파일 병행 가능.
- **파서 백엔드 교체 가능** — 지금은 경량 TOML/INI-subset, 스키마(EngineConfig) 고정.

## 미완료 / 다음 단계로 이월
- **Docker Linux 이미지** — 엔진이 Windows 전용인 동안 불가. **Phase 4(epoll)**에서 Linux 빌드 활성화 후 멀티스테이지 Linux 이미지.
- **SIGTERM graceful drain** — 종료 시 잔여 완료 끝까지 배수 + 신규 차단(현재 Stop은 신호+CloseAll까지). Phase 4.
- **설정 핫리로드** — 안전 항목(로그 레벨, 타임아웃)만 파일감시/시그널 재적용. 후속.
- **비동기 로거** — 현재 mutex 동기. 고부하 시 큐 기반으로.
- **health/readiness 체크** — 컨테이너 오케스트레이션용(TCP 체크 또는 엔드포인트).

## 검증 상태
**미검증(컴파일 안 함).** Windows/MSVC 전용. VS(x64) 빌드 후:
- 설정: `helix.exe`가 `config/helix.toml` 로드, `HELIX_NETWORK__PORT=6000 helix.exe`로 환경변수 오버라이드 확인. 잘못된 값(예: `backend=foo`)이면 기동 거부 + 에러 로그.
- 로그: stdout에 `타임스탬프 LEVEL msg` 출력. `HELIX_LOGGING__LEVEL=debug`로 레벨 변경.
- 봇: 기존 `tools/echo_bot.py`로 에코/churn 검증(설정 바뀐 포트 반영).

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md
- 이전 단계: ./PHASE2.md
