# Helix 문서 안내

현재 빌드와 실행 절차는 [루트 README](../README.md)를 기준으로 한다. C++과 C#은 독립 엔진이며, C#은 .NET 10 TCP 엔진이다. C++ C ABI는 선택 빌드다.

| 문서 | 내용 |
|---|---|
| [MMO.md](MMO.md) | 인스턴스 조립, 이동·전투·AOI 주입, tick 경계 교체 |
| [PARALLEL_MMO.md](PARALLEL_MMO.md) | C++ 작업 그래프 병렬 월드, 단계별 전투 의미, 라이브러리 비교·검증 |
| [SERIALIZATION.md](SERIALIZATION.md) | Protobuf·FlatBuffers·MemoryPack 계약과 호환성 |
| [CONFIGURATION.md](CONFIGURATION.md) | 파일·환경변수 설정과 MMO 한도 |
| [CODE_MAP.md](CODE_MAP.md) | 파일·클래스의 책임과 엔진 간 경계 |
| [API.md](API.md) | C++ 공개 SDK와 선택 C ABI 레퍼런스 |
| [USAGE.md](USAGE.md) | 현재 C++/C# 서버 작성·실행·설치 SDK 사용 |
| [BUILD.md](BUILD.md) | 독립 빌드, 선택 모듈, 실제 통신·설치 소비자 검증 |
| [개발 계획](../HELIX_ENGINE_PLAN.md) | 설계 방향과 단계별 목표 |
| [Phase 기록](phases/) | 단계별 변경·검증·이월 항목, 최신 기록은 [PHASE10.md](phases/PHASE10.md) |

처음 실행할 때는 빌드·사용 가이드에서 시작하고, 콘텐츠를 조립할 때 MMO·병렬 MMO·직렬화 계약을 확인한다. C++ 병렬 모듈의 기본값은 OFF이며 C#은 별도 개발 범위다. 현재 단순 규칙의 성능 향상은 확인되지 않았고 측정 조건과 수치는 병렬 MMO 문서에 있다.

Phase 기록과 개발 계획의 초기 v2 자료에는 당시 P/Invoke 구조·RIO 미구현·과거 패키지 버전이 남아 있다. 이력으로 읽고 현재 사용 절차로 적용하지 않는다. [TODO.md](TODO.md)와 [고성능 구조 검토](REPORT-high-perf-arch.md)도 초기 설계 자료로 보존한다.

벤치마크 원시 결과와 보고서는 로컬 `out/`에 생성한다. 생성 명령과 측정 범위는 루트 README의 성능 비교 절을 따른다. 실행별 결과는 저장소에 포함하지 않는다.
