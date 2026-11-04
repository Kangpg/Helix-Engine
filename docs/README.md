# 문서 안내

Helix는 연결·세션·메시지 송수신을 제공하는 범용 네트워크 서버 엔진이다. C++과 C#은 독립적으로 사용한다.

## 기본 사용

| 문서 | 확인할 내용 |
|---|---|
| [README](../README.md) | 프로젝트 소개와 빠른 시작 |
| [빌드·검증](BUILD.md) | Windows/Linux C++, 독립 C#, 테스트·설치 |
| [사용 가이드](USAGE.md) | 서버 작성, 설정, 전송 선택, 외부 프로젝트 연결 |
| [API](API.md) | C++ SDK·선택 C ABI·C# 공개 API |
| [설정](CONFIGURATION.md) | 파일·환경변수, 연결·버퍼·프레이밍·로그 |
| [성능 측정](BENCHMARKS.md) | 네트워크·직렬화·콘텐츠 측정 도구와 재현 명령 |

## 확장 모듈과 예제

앱은 엔진 위에 자체 메시지와 처리 로직을 조립한다. 아래 문서는 저장소에 포함된 예제 모듈을 다룬다.

| 문서 | 확인할 내용 |
|---|---|
| [예제 메시지 직렬화](SERIALIZATION.md) | Protobuf·FlatBuffers·C# MemoryPack의 메시지 계약 |
| [MMO 콘텐츠](MMO.md) | 이동·전투·가시성 규칙과 상태 관리 |
| [C++ 병렬 월드](PARALLEL_MMO.md) | 작업 그래프 콘텐츠 처리 실험과 측정 한계 |
| [테스트 GUI](../tools/gui/README.md) | 검증 도구 실행과 결과 확인 |

## 구현과 이력

[코드 지도](CODE_MAP.md)는 파일과 클래스의 역할을 설명한다. [개발 계획](../HELIX_ENGINE_PLAN.md)과 [Phase 기록](phases/)은 설계·변경·검증 이력이다. 최신 후속 기록은 [PHASE10.md](phases/PHASE10.md)에 있다.

초기 계획·[TODO](TODO.md)·[구조 검토](REPORT-high-perf-arch.md)에는 당시의 미구현 기능과 이전 언어 구조가 남아 있다. 현재 사용 절차와 지원 범위는 위 기본 사용 문서를 따른다. 실행별 빌드·측정 산출물은 로컬 `build/`·`out/`에 생성하며 Git에는 포함하지 않는다.
