# Phase 10 — MMO 인스턴스 조립 및 C++·C# 독립 엔진 분리

> 기록일: 2026-10-12 (Asia/Seoul) · 상태: 완료 (이번 단계 범위). 대규모 MMO 최적화와 추가 전송·플랫폼 검증은 후속.

## 목표

[계획의 현재 방향](../../HELIX_ENGINE_PLAN.md)에 따라 C++과 순수 C# 엔진을 독립 프로젝트로 분리한다. 인스턴스마다 이동·전투·AOI 기능을 작은 인터페이스 또는 델리게이트 어댑터로 주입·교체하고, 네트워크 입력과 MMO 상태 변경의 책임을 분리한다. QUIC은 후순위로 둔다.

기존 Phase 0~9의 목표·검증·완료 상태는 당시의 이력으로 보존한다. 특히 Phase 8~9의 C# P/Invoke 래퍼·네이티브 DLL 동봉 방식은 이번 단계에서 순수 C# 엔진으로 대체했다.

## 완료한 작업

### 독립 프로젝트와 API 경계

- **신규 조립 / 기존 빌드 수정**: `cpp/CMakeLists.txt` 아래 `Helix.Engine`과 `Helix.Mmo`를 조립한다. 루트 CMake와 `windows-x64` 프리셋도 같은 C++ 프로젝트를 사용한다. 기존 네트워크 소스는 루트 `include/`·`src/`에서 재사용한다.
- **신규**: `csharp/Helix.sln`에 순수 .NET 8 `Helix.Engine`·`Helix.Mmo`, 에코·월드 데모·월드 서버 샘플 및 테스트를 구성했다. C#은 C++ DLL 없이 비동기 TCP로 실행한다.
- **기존 경계 선택화**: C++ `helix_capi`는 `HELIX_BUILD_CAPI=ON`일 때만 빌드한다. 기본값은 OFF이며, C# 엔진이 C ABI를 사용하지 않는다. 다른 언어의 C ABI 경계에서는 기존 예외 비전파 계약을 유지한다.
- **신규 / 수정**: C++ `helix::helix`·`helix::mmo` install/export와 순수 C# `Helix.Engine`·`Helix.Mmo` NuGet 패키징을 연결했다.

### 인스턴스별 MMO 기능

- **신규 C++**: `cpp/Helix.Mmo/include/helix/mmo/`의 `IMovementPolicy`, `ICombatRules`, `IVisibilityPolicy`, `DelegateMovementPolicy`, `DelegateCombatRules`, `DelegateVisibilityPolicy`, `WorldInstance` 및 `src/WorldInstance.cpp`.
- **신규 C#**: `csharp/Helix.Mmo/`에 같은 역할의 인터페이스, `DelegatePolicies`, `WorldBehaviors`, `WorldInstance` 및 불변 값·스냅샷 타입.
- `QueueAdd`·`QueueRemove`·`QueueMove`·`QueueAttack`은 바운드 큐로 요청을 받는다. `WorldLimits`가 최대 엔티티·대기 명령·tick당 처리량을 제한하며, 큐 초과와 규칙 거부를 구분한다.
- `Tick`만 엔티티 상태를 변경한다. 동시/재진입 tick을 거부하고, tick 시작 시 명령 배치와 pending 기능 교체를 확정한다. 실행 중 들어온 입력·교체는 다음 tick에 적용한다.
- `ReplaceMovement`·`ReplaceCombat`·`ReplaceVisibility`는 해당 인스턴스의 구현만 교체한다. 같은 역할의 여러 교체 요청 중 마지막 pending 구현을 적용한다.
- 완료한 tick의 불변 스냅샷과 AOI 구현을 함께 게시한다. 진행 중 조회는 잡아둔 게시 객체를 사용하여 데이터·규칙의 버전과 수명을 유지한다. AOI 결과의 자기 자신·중복·존재하지 않는 ID는 걸러낸다.
- 이동/전투 정책은 새 위치 또는 피해량을 계산한다. 월드가 출력의 유효성을 확인하고 상태를 변경한다. 규칙 예외나 유효하지 않은 출력은 해당 명령을 거부한다.

### 관리 네트워크와 샘플 연결

- **신규**: C# `Transport/TcpAcceptor`, `TcpSession`, `TcpServerTransport`, `Protocol/FrameCodec`, `SendQueue`로 수락·연결 수명·프레이밍·송신을 나누었다. `IServerTransport`·`ISessionTransport`로 외부 전송을 주입할 수 있다.
- **적용**: .NET `System.Threading.Channels` 바운드 송신 큐를 사용한다. 단일 reader가 송신을 직렬화하며, `Wait` 모드의 `TryWrite` 실패로 큐 포화를 보고한다. 조용히 메시지를 버리는 Drop 모드는 사용하지 않는다.
- **설정 수정**: 양쪽 `EngineConfig` 및 `config/helix.toml`의 `[mmo]`에 엔티티/큐/tick 처리 한도와 tick 주기를 외부화했다. 파일 설정에 환경변수 우선순위를 적용한다.
- **신규**: `cpp/Helix.Mmo/samples/server.cpp`와 `csharp/samples/Helix.WorldServer/Program.cs`는 동일한 4바이트 big-endian 프레이밍과 `SID`·`MOVE`·`ATTACK`·`LOOK` 샘플 프로토콜을 사용한다. 필드와 투기장은 다른 전투 정책을 주입하며, 조회는 완료한 tick의 스냅샷을 읽는다.
- **신규 검증**: `cpp/Helix.Mmo/tests/world_instance_test.cpp`, `csharp/tests/Helix.Tests/Program.cs`, `tools/world_bot.py`, `tools/verify_engines.py`.
- **CI 수정 / 신규**: Linux C++ CI에 CTest 실행을 추가하고, C# CI에 Ubuntu·Windows 솔루션 빌드와 관리 테스트 콘솔을 연결했다. 워크플로 자체의 실제 CI 실행은 이번에 검증하지 않았다.
- **문서 / 스킬 갱신**: `docs/CODE_MAP.md`·`docs/MMO.md`에 독립 엔진 구조와 Mermaid 그림, C++ `lower_bound`·C# 선형 조회의 차이를 반영했다. 프로젝트 스킬 10개와 AGENTS 라우팅을 현재 구조에 맞게 갱신했다.

### 정리

- 사용하지 않는 `vcpkg.json`·`.gitmodules`와 vcpkg 프리셋을 제거했다. 현재 C++ 빌드는 vendored KCP 소스를 사용한다.
- C# P/Invoke 소스와 네이티브 DLL을 동봉하던 `Helix.Sdk` 프로젝트를 제거·대체했다.
- QUIC 구현을 추가하지 않았다. C++ 기존 TCP·KCP·Redis는 유지하며, 예약된 전송 값은 동작 구현으로 소개하지 않는다.
- 과거 `bin`·`obj`·빌드 산출물 삭제는 자동 승인 검토가 위험을 이유로 거절하여 보존했다. 남은 과거 `helix.dll` 파일은 현재 C# 프로젝트의 참조나 패키지 의존성이 아니다.

## 설계 결정

- **S / I**: 통신 엔진과 시뮬레이션 라이브러리를 분리하고, 이동·전투·가시성마다 작은 계약을 둔다. MMO 모듈이 네트워크 연결이나 프로토콜을 소유하지 않는다.
- **O / D**: `WorldInstance`는 `WorldBehaviors`의 추상 계약으로 동작한다. 기능 확장은 새 클래스나 델리게이트 어댑터를 주입하며, 인스턴스 내부에 구체 규칙을 고정하지 않는다. C# 호스트도 외부 전송을 주입할 수 있다.
- **L**: 클래스와 델리게이트가 같은 입력·거부·출력 계약을 지킨다. 정책은 상태를 직접 변경하지 않으며 이동 결과는 유한 좌표, 전투 결과는 비음수 피해량이어야 한다.
- 네트워크 콜백은 월드 큐에 입력한다. 큐 접수 성공은 실제 적용 완료를 뜻하지 않는다. 실시간 교체는 다음 tick 시작 경계에만 적용한다.
- C++ 정책 수명은 `shared_ptr<const interface>`, C# 정책 수명은 참조로 보장한다. 이는 MMO 정책/스냅샷의 소유권이며 기존 네트워크 세션의 단일 참조카운트 계약과 별개다.
- AOI는 여러 조회 스레드에서 호출될 수 있다. 공유 정책과 델리게이트 캡처의 동시 안전성은 구현자가 보장하며, C#의 외부 `IDisposable` 자원은 진행 중 조회가 끝난 후 소유자가 정리한다.
- C++/C#은 구현과 빌드를 독립적으로 유지하고, MMO 계약·프로토콜·검증 시나리오를 맞춘다. 독립 엔진은 모든 전송·클러스터 기능의 동등 지원을 뜻하지 않는다.

## 미완료 / 다음 단계로 이월

- **후속 기능**: 순수 C# KCP·Redis·TLS는 미구현이다. QUIC 및 실제 RIO 데이터 경로도 후순위로 유지한다.
- **후속 성능**: 명령 평가 시 엔티티 뷰 복사와 전체 스냅샷 게시를 사용한다. C++의 정렬 벡터 조회는 `lower_bound`, C# 조회는 선형 탐색이며, 샘플 AOI는 인스턴스 내 전체 가시성이다. 공간 인덱스·할당 감소·배치 처리·대규모 동접 부하는 이번 완료 범위에 포함하지 않는다.
- **운영 앱 후속**: 인증, 이동 충돌, 영속 데이터, 월드 마이그레이션, 접속 재개는 구현하지 않았다. 샘플 종료 콜백에서 큐 포화로 엔티티 제거가 실패하면 로그로 보고하며, 운영 앱은 생명주기 명령 재시도/수락 제한을 설계해야 한다.
- **플랫폼 검증**: Windows loopback에서 검증했다. Linux 런타임·Docker 실행·다중 노드 Redis·KCP 회귀 부하를 이번 실행 증거로 주장하지 않는다.
- **Phase 9 이월**: Windows install/export 소비자 검증과 새 C# NuGet 생성은 이번에 확인했다. Linux 설치·컨테이너 매트릭스, 벤치마크 CI, NuGet 피드 게시/버전 정책은 계속 후속이다. 패키지 생성은 게시 완료를 뜻하지 않는다.
- `/docs/`는 기존 `.gitignore` 대상이다. 이 기록은 작업 공간에 저장하며 요청 없이 ignore 정책을 바꾸지 않았다. 작업 시작 전부터 소스 대부분이 untracked였으므로 `git diff --stat`만으로 이번 변경량을 판단하지 않는다.

## 검증 상태

아래 실행 결과는 이번 작업의 주 에이전트가 제공한 검증 증거다. phase-logger는 실제 코드·테스트·현재 CMake cache·CTest 로그·소비자 소스·패키지 산출물과 교차 확인했으며, 기록을 위해 빌드/서버를 다시 실행하지 않았다.

| 검증 | 실행 / 결과 |
|---|---|
| 정본 C++ 빌드 | `cmake --preset windows-x64`, `cmake --build --preset windows-debug` 성공. 기본 C ABI OFF |
| C++ MMO 계약 | `ctest --test-dir build/windows-x64 -C Debug --output-on-failure`: `world-instance` PASS. 격리·클래스/델리게이트·세 기능 교체·tick 중 교체·동시 tick 거부·스냅샷·한도·규칙 실패 |
| 독립 C++ 프로젝트 | `cmake -S cpp -B build/cpp-independent -G "Visual Studio 17 2022" -A x64 -DHELIX_BUILD_CAPI=ON`, `cmake --build build/cpp-independent --config Debug` 성공. 엔진·MMO·샘플·테스트와 선택 C ABI `helix.dll`·`helix_capi.lib` 생성 |
| 독립 C# 솔루션 | `dotnet build csharp/Helix.sln -c Release --disable-build-servers -m:1 -v:minimal`: 6개 프로젝트 성공, 경고 0·오류 0 |
| 관리 테스트 콘솔 | `dotnet csharp/tests/Helix.Tests/bin/Release/net8.0/Helix.Tests.dll` PASS. MMO 계약 외 Channels 포화, 외부 전송 주입, 분할/합친 프레임, 4바이트 big-endian 및 2바이트 little-endian, 과대 패킷 차단, 종료 세션 회수·콜백 1회, 설정 우선순위/오류 |
| 양쪽 TCP echo/churn | 최신 `python tools/verify_engines.py` 전체 실행 exit 0. 관리 계약 테스트와 C++·C# 각각 echo 20 clients × 50 messages = 1,000회, churn 20 clients × 10 rounds = 200회 PASS |
| 공통 MMO 프로토콜 | 같은 검증 도구가 양쪽 월드 서버에 `world_bot.py`를 실행: 이동, 필드 공격 거부, 투기장 피해, 인스턴스별 AOI 격리 PASS |
| 설치 소비자 | `cmake --install build/windows-x64 --config Debug --prefix out/native-current` 성공. 아래 소비자 구성/빌드/실행 exit 0. `find_package(helix CONFIG REQUIRED)`로 `helix::helix`와 `helix::mmo`에 링크 |
| 선택 C ABI 설치 | 독립 C++ 선택 C ABI 빌드의 별도 install이 `out/refactor-sdk`에 성공 |
| 순수 C# 패키징 | `dotnet pack csharp/Helix.Engine/Helix.Engine.csproj -c Release --no-build -o out/packages` 및 `dotnet pack csharp/Helix.Mmo/Helix.Mmo.csproj -c Release --no-build -o out/packages` 성공. `Helix.Engine.0.2.0.nupkg`·`Helix.Mmo.0.2.0.nupkg` 생성. 두 ZIP 내용 검사로 관리 DLL·README 포함 및 네이티브 `helix.dll`·`runtimes` payload 부재 확인 |
| 프로젝트 스킬 | 10개 `quick_validate`, UI 메타데이터, 상대 링크 30개 및 AGENTS route coverage 검증 PASS |
| CI 구성 | Linux C++ CTest 및 Ubuntu·Windows C# 빌드/콘솔 테스트 경로를 연결. 실제 CI 실행은 미검증 |

설치 경계 검증에 사용한 외부 소비자 명령:

```powershell
cmake -S build/refactor-consumer -B build/refactor-consumer/output -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=D:/git/helix-engine/out/native-current
cmake --build build/refactor-consumer/output --config Debug
.\build\refactor-consumer\output\Debug\consumer.exe
```

실행 로그는 `build/windows-x64/Testing/Temporary/LastTest.log`와 `build/verification/{cpp,csharp}-{echo,world}.log`에 남는다. 빌드 산출물 및 문서 저장은 Git 추적/외부 게시와 구분한다.

## 관련 문서

- 계획: [HELIX_ENGINE_PLAN.md](../../HELIX_ENGINE_PLAN.md)
- 이전 단계: [PHASE9.md](./PHASE9.md)
- 사용·빌드: [README.md](../../README.md) · [BUILD.md](../BUILD.md)
- MMO 계약·프로토콜: [MMO.md](../MMO.md)
- API·설정·코드 지도: [API.md](../API.md) · [CONFIGURATION.md](../CONFIGURATION.md) · [CODE_MAP.md](../CODE_MAP.md)

## 후속 추가 — 2026-10-12 공통 Protobuf 메시지

> 상태: 완료 (이번 Protobuf 추가 작업 범위). 위의 독립 엔진 리팩토링 본문과 당시 검증 결과는 이력으로 유지하며, 원격 CI·Linux/Docker와 운영 규모 검증은 후속이다.

### 목표와 완료한 작업

- [계획의 현재 방향](../../HELIX_ENGINE_PLAN.md)에 따라 공통 [helix_mmo.proto](../../protos/helix_mmo.proto)를 정본으로 추가했다. `ClientPacket`·`ServerPacket`의 버전·`request_id`·`oneof`, 이동·공격·조회 명령, 환영·접수 결과·월드 상태 메시지를 정의한다. 필드 번호는 재사용하지 않는다.
- **신규 독립 모듈**: `cpp/Helix.Protocol`의 `helix::protocol` 타깃과 `csharp/Helix.Protocol`에 생성 메시지 및 Codec을 둔다. C++ 생성 헤더·schema를 install/export하고, C#은 별도 NuGet으로 패키징한다.
- **샘플 수정**: 양쪽 WorldServer의 이전 텍스트 메시지를 4바이트 big-endian 길이 + Protobuf payload로 교체했다. 엔진의 프레이밍·바이트 메시지 계약과 MMO 도메인 규칙은 유지한다. Engine/Mmo 라이브러리는 Protobuf에 직접 의존하지 않고, WorldServer 앱이 네트워크·직렬화·월드를 조립한다.
- **검증 계약**: 파싱 실패, 지원하지 않는 버전, 누락된 명령, 이동 목적지 누락/비유한 좌표, 공격 대상 0을 거부한다. Protobuf `oneof`는 메시지 변형을 표현하며, 모르는 추가 필드는 허용한다. 앱이 세션 ID를 도메인 요청의 행위자 ID로 주입한다.
- 파싱된 요청은 응답에서 `request_id`를 보존한다. 파싱 실패 응답은 ID 0과 INVALID를 사용한다. 이 ID는 응답 상관관계이며 인증·중복 실행 방지를 제공하지 않는다. QUEUED는 큐 접수이고 규칙 적용 완료가 아니다.
- **빌드 선택화**: `HELIX_BUILD_PROTOCOL` 기본값은 ON이며 OFF로 Protocol 및 Protobuf 월드 서버 샘플을 제외할 수 있다. 기존 엔진·오프라인 월드 데모·MMO 테스트는 유지한다. C ABI 선택은 별도 옵션이다.
- **의존성 고정**: C++ Protobuf runtime 36.2와 공식 `protoc` 36.2를 정확히 맞춘다. 설치된 runtime에 compiler가 없으면 `HELIX_PROTOC_EXECUTABLE`로 같은 버전의 host compiler만 주입할 수 있다. C#은 `Google.Protobuf` 3.36.2와 `Grpc.Tools` 2.84.0을 사용하며 `PrivateAssets="all"`·`GrpcServices="None"`으로 메시지만 생성한다. Python 검증 runtime은 `protobuf==7.36.2`다.
- **검증 도구 수정**: `tools/generate_protocol.py`가 같은 schema에서 Python 메시지를 생성한다. `world_bot.py`가 양쪽 서버의 정상 동작과 malformed/version/명령/finite/unknown-field/request-ID 계약을 검증하며, `verify_engines.py`가 생성과 기존 echo/churn을 함께 실행한다.
- **문서 / 스킬 / CI 수정**: README·MMO·CODE_MAP의 메시지 경로와 구조 그림을 갱신했다. 새 `helix-protocol` 및 라우팅 관련 7개 파일을 추가·갱신했으며 스킬은 11개다. Windows·Linux CI에 두 언어를 같은 생성 클라이언트로 검증하는 단계를 추가했다.

### 설계 결정과 범위

- 직렬화 모듈은 전송과 월드 상태의 소유자가 아니다. C++ IOCP/epoll을 유지하고 Asio를 도입하지 않았다. gRPC 서비스나 QUIC 구현도 추가하지 않았으며, QUIC 후순위 및 순수 C# KCP·Redis·TLS 미구현 상태는 유지한다.
- WorldState의 위치·HP·tick은 같은 `Snapshot`에서 읽는다. `VisibleTo`는 별도 게시 뷰 조회여서 tick 경계에서 다음 tick의 AOI를 반환할 수 있다. 전체 응답의 단일 tick 원자성을 보장하려면 월드 통합 조회 계약을 후속으로 확장해야 한다.
- 생성 코드는 빌드 산출물이고 schema가 정본이다. 설치 패키지의 Protobuf dependency와 C# 런타임 dependency를 명시하며, `Grpc.Tools`는 NuGet 소비자에게 전파하지 않는다.
- 최초 CMake 다운로드의 TLS 신뢰 실패는 Windows의 기존 신뢰 Root를 `build/windows-ca.pem`으로 내보내고 `CMAKE_TLS_CAINFO`로 지정하여 해결했다. TLS 검증을 끄지 않았다. 패키지 복원·공식 protoc 다운로드·localhost TCP 검증은 sandbox escalation의 정상 승인으로 완료했다.

### 추가 검증 상태

다음은 주 에이전트가 제공한 실제 실행 결과를 schema·Codec·테스트·CMake cache·CTest 로그·CI 정의·패키지 및 라이선스 산출물과 교차 확인한 기록이다. 이전 표의 6개 C# 프로젝트·텍스트 봇·스킬 10개 결과는 그 시점의 이력이며, 아래 결과가 추가 작업 후의 상태다.

| 검증 | 실행 / 결과 |
|---|---|
| 정본 C++ | `cmake --preset windows-x64`, `cmake --build --preset windows-debug` 성공. Protobuf 생성 및 Protocol 포함 |
| C++ 계약 | `ctest --test-dir build/windows-x64 -C Debug --output-on-failure`: `protocol-buffers`·`world-instance` 2/2 PASS |
| C# 솔루션 | Release 7개 프로젝트 빌드 성공, 경고 0·오류 0. 관리 콘솔의 Protobuf 계약 및 기존 네트워크/MMO 테스트 PASS |
| 언어 간 실행 | `python tools/verify_engines.py` 성공. C++·C# 각각 echo 1,000회/churn 200회, 같은 Python 생성 클라이언트로 이동·필드 공격 거부·투기장 전투·AOI 격리 및 malformed/version/NaN/누락 명령·목적지/0 대상/unknown field/request ID PASS |
| native 설치 / 소비자 | CMake install로 `out/protobuf-sdk` 생성. `build/protobuf-consumer`의 외부 `find_package(helix)` 소비자가 `helix::protocol`에 링크하여 빌드·실행 PASS |
| 설치 runtime 재사용 | 독립 `cpp` 프로젝트를 `build/protobuf-installed-build`에 구성하여 설치 Protobuf runtime과 주입된 동일 버전 protoc로 Protocol 테스트 빌드·실행 PASS |
| Protocol 제외 조합 | Protobuf `_deps` 없는 독립 `cpp` 구성에서 `HELIX_BUILD_PROTOCOL=OFF`·`HELIX_BUILD_CAPI=ON`. `cmake --build build/protobuf-core-only --config Debug --parallel 4 -- /nodeReuse:false` exit 0, CTest `world-instance` 1/1 PASS. 엔진·MMO·샘플·`helix.dll`·`helix_capi.lib` 생성 확인. 기존 `helix.pdb` 누락 LNK4099 경고가 있어 무경고 빌드로 표기하지 않는다. 최초 sandbox 내부 빌드 대기는 해당 세션 Ctrl+C 종료 후 escalation 재실행으로 완료 |
| Protocol NuGet | `Helix.Protocol.0.2.0.nupkg`의 관리 DLL·schema·README와 `Google.Protobuf` 런타임 dependency 확인. `Grpc.Tools` dependency·native payload 없음 |
| third-party 고지 | `out/protobuf-sdk/share/helix/licenses`의 `protobuf-LICENSE`·`utf8_range-LICENSE`·`abseil-LICENSE` 포함 확인 |
| 스킬 | 11개 스킬 `quick_validate`, UI implicit 메타데이터 및 모든 상대 링크 검증 PASS |
| CI / 플랫폼 | Windows·Linux cross-language 단계 연결 및 workflow YAML 파싱 PASS. 원격 CI·Linux 런타임·Docker는 이번에 실행하지 않음 |

### 미완료 / 이월

선택 빌드 조합의 검증은 완료했다. 원격 CI·Linux/Docker 실행, 대규모 MMO 최적화, 통합 상태/AOI 조회, 순수 C# 추가 전송·클러스터 구현은 후속 범위다. Protobuf 채택 자체는 인증·중복 요청 제거·암호화·운영 규모 검증을 제공하지 않는다. 기존 docs Git ignore, 작업 시작 전 untracked 소스, 과거 생성 산출물 삭제 거절/보존 이력은 그대로 유지한다.

## 후속 추가 — 2026-10-12 직렬화 비교 및 IOCP/RIO 하이브리드

> 상태: 완료 (이번 구현·회귀·예비 비교 범위). BenchmarkDotNet 정식 측정과 원격 CI·Linux/Docker 실행은 후속이다. 위 본문의 RIO 미구현/후순위 설명은 당시 이력이며, 이번 절이 현재 Windows RIO 계약을 기록한다.

### 목표와 완료한 작업

- [계획의 현재 방향](../../HELIX_ENGINE_PLAN.md)에 따라 운영 wire는 Protobuf로 유지하면서 독립 benchmark에서 포맷별 크기·encode/decode·관리 할당량을 비교했다. Windows `iocp_rio`를 실제 RIO 송수신 경로로 구현하고 세션·수락·종료의 수명 계약을 검증했다.
- **신규 백엔드**: `src/net/WindowsRioBackend`는 `INetBackend`를 구현하며 `WindowsIocpBackend`와 `RioDataPlane`을 조립한다. AcceptEx와 완료 통지는 IOCP, 실제 송수신은 RIO가 맡는다. 코어는 구체 백엔드 대신 팩토리·인터페이스를 사용한다.
- **신규 데이터 경로**: `RioDataPlane`이 CQ/RQ·`RIONotify`·IOCP notification을 관리한다. `RioBuffer`는 `VirtualAlloc`/등록 버퍼를 컨텍스트별로 재사용하며, 등록 저장소를 취소 완료까지 유지한다. 송수신 staging copy와 공유 RQ/CQ 락을 사용하는 구현이다.
- **수락 / 세션 수정**: 추상 소켓 생성·종료 hook으로 listen/accept 소켓의 RIO flags를 맞춘다. Session은 부분 송신 offset을 누적하고 남은 바이트를 재게시하여 프레임 순서를 유지한다. post/close를 mutex로 직렬화하며 I/O 실패·완료의 참조를 락 밖에서 `Release`한다.
- Acceptor는 pending AcceptEx 취소 완료까지 컨텍스트를 유지한다. 앱 수락 콜백은 accept 락 밖에서 호출하며, ServerCore가 세션 및 수락의 취소 완료를 배수한 후 소유자를 파괴한다.
- **선택 계약 수정**: Windows `auto`·`iocp`는 일반 IOCP다. 명시적 `iocp_rio`는 실제 RIO 초기화에 성공해야 시작하며, 미지원/자원 부족을 IOCP 폴백으로 숨기지 않는다. 실제 활성 이름은 `iocp+rio`다. QUIC은 계속 후순위다.
- **설정 추가**: `[rio].buffer_size=16384`, `completion_queue_size=32768` 및 `HELIX_RIO__BUFFER_SIZE`·`HELIX_RIO__COMPLETION_QUEUE_SIZE` 환경변수. 엄격한 양의 정수 파싱과 RIO 선택 시 `completion_queue_size >= 2 * max_connections`를 검증한다.
- **종료 계약**: Windows drain budget 초과 시 경고를 출력한 후 안전한 취소 완료 회수를 계속한다. 이 값은 강제 종료 hard deadline이 아니다. Linux는 기존 timeout 경로를 유지했으며 이번 런타임 검증 대상은 아니다.
- **신규 테스트 / 도구 수정**: `tests/session_partial_test.cpp`, `tests/windows_backend_test.cpp`와 CTest 타깃을 추가했다. `verify_engines.py --native-backend`는 요청한 실제 backend 이름도 확인한다.

### 독립 직렬화 benchmark

- **신규 C#**: `csharp/benchmarks/Helix.SerializationBenchmarks`는 별도 실행 프로젝트다. 엔진·MMO·정상 `Helix.sln` 빌드에 비교 패키지 의존성을 추가하지 않는다. Protobuf·MessagePack·MemoryPack·source-generated JSON·FlatBuffers·fixed Binary 6개 포맷을 같은 `State`로 비교한다.
- benchmark에서 사용하는 버전은 `Google.Protobuf` 3.36.2(Protocol 참조), `MessagePack` 3.1.11, `MemoryPack` 1.21.4, `Google.FlatBuffers` 25.2.10 및 `BenchmarkDotNet` 0.15.8이다. 운영 메시지의 패키지/프로토콜 선택은 바꾸지 않았다.
- **신규 C++**: `cpp/benchmarks`는 Protobuf·FlatBuffers·fixed Binary 3개 포맷을 비교한다. `HELIX_BUILD_BENCHMARKS` 기본값은 OFF이며 Protocol ON과 `flatc` 25.2.10이 필요하다. FlatBuffers source는 버전·SHA256으로 고정하고 C++ 생성 헤더는 build 디렉터리에 둔다.
- 비교용 정본은 `csharp/benchmarks/Helix.SerializationBenchmarks/world_state.fbs`다. C# Generated는 재생성 가능한 benchmark 소스이며 `tools/generate_benchmark_schema.ps1`로 갱신한다. FlatBuffers bulk vector/array API를 사용한다.
- 동일 entity ID·double 좌표 3개·HP·tick·AOI ID 0/16/256/4096개를 사용한다. encode는 새 소유 bytes를 생성하고 decode는 전체 DTO·ID 배열을 복원하며, 도메인/생성 타입 mapping 비용을 포함한다. 버퍼 view만 읽는 비용을 전체 복원과 섞지 않는다.
- 이 비교는 올바른 입력의 FlatBuffers decode이며 별도 verifier 비용을 제외한다. 관리 할당량은 managed B/op이고 native 전체 메모리와 같은 지표가 아니다. MemoryPack 결과는 C# 포맷의 결과이며 fixed Binary는 스키마 진화 기능 없는 비교 기준이다.
- **예비 순차 측정**: Windows x64, MSVC C++ Release, .NET 8.0.29 Release, 논리 CPU 16개. warmup 2,000회 후 C#은 tiered compilation=0·9 batches × 5,000 operations, C++은 9 batches × 10,000 operations의 중앙값이다. BenchmarkDotNet 정식 실행 경로는 구현했지만 이번에 실행하지 않았다.
- 원시 결과는 [serialization-managed.json](../../out/serialization-managed.json)·[serialization-native.csv](../../out/serialization-native.csv), 통합 예비 보고서는 [serialization-report.md](../../out/serialization-report.md)다. `tools/serialization_report.py`가 실제 원시 값을 보고서로 옮긴다. managed 24 cases·native 12 cases를 확인했다.

### IOCP/RIO 비교와 해석

`tools/backend_benchmark.py`는 같은 native Release TCP echo 서버에 I/O threads 4개, 50 clients × 500 messages, payload 256/8192 bytes, 각 3 samples를 적용했다. 측정 순서를 번갈아 실행하고 로그의 실제 `iocp`·`iocp+rio` 이름을 확인했다. [backend-benchmark.json](../../out/backend-benchmark.json)의 12개 측정 행은 모두 PASS다.

| payload bytes | IOCP median msg/s | IOCP+RIO median msg/s |
|---:|---:|---:|
| 256 | 28,610 | 28,794 |
| 8192 | 22,822 | 23,671 |

Python asyncio 클라이언트·TCP 프레이밍·loopback·스케줄링을 포함한 예비 결과다. 순수 커널 I/O 비용이나 대규모 MMO 처리량이 아니며, 통계적 유의성 또는 RIO가 항상 빠르다는 근거로 해석하지 않는다. helper의 프로세스 종료는 graceful shutdown 증거가 아니며, 정상 취소 배수는 별도 Windows CTest에서 확인했다.

### 검증 상태

주 에이전트의 최종 실행 결과를 실제 구현·테스트·CMake cache·CTest 및 최종 빌드/검증 로그·원시 benchmark 결과와 교차 확인했다. 이전 절의 2개 CTest·스킬 11개 결과는 당시 이력으로 유지한다.

| 검증 | 실행 / 결과 |
|---|---|
| 최종 정본 | `cmake --preset windows-x64 -DHELIX_BUILD_BENCHMARKS=OFF` 및 Debug 전체 빌드 성공. 최종 benchmark cache는 OFF로 복원 |
| 최종 CTest | `ctest --test-dir build/windows-x64 -C Debug --output-on-failure` 4/4 PASS: `session-partial-send`·`windows-iocp-rio`·`protocol-buffers`·`world-instance` |
| 부분 송신 계약 | 순서 보존·재게시 실패·수신 취소 후 단일 참조 및 context 회수 PASS |
| 실제 Windows 백엔드 | IOCP·IOCP+RIO 각각 200KB frame 왕복, 부분 송신, pending recv/AcceptEx를 가진 정상 종료·세션 회수 PASS. CQ 한도 부족 및 buffer 0은 초기화 실패하며 폴백하지 않음 |
| 최종 통합 회귀 | `python tools/verify_engines.py --native-backend iocp_rio` exit 0. C++ actual `iocp+rio` 확인, C++·C# 각각 echo 1,000회/churn 200회, 양쪽 MMO 공통 Protobuf 및 관리 계약 테스트 PASS |
| 직렬화 비교 | 독립 C# benchmark 및 선택 C++ benchmark Release 빌드/전체 복원 round-trip/예비 측정 성공. 원시 managed 24 cases·native 12 cases 및 backend 12 PASS rows 확인 |
| 스킬 / 문서 / CI 정의 | `helix-benchmark` 추가로 스킬 12개. quick_validate·상대 링크·UI implicit metadata·CI YAML 검증 PASS. README·계획·설정·MMO·CODE_MAP 갱신 |
| CI 실행 범위 | Windows 실제 RIO 및 OS별 benchmark round-trip 단계를 연결했으나 원격 CI·Linux 런타임·Docker는 미실행 |

최종 실행 로그: `build/rio-final-build.log`, `build/rio-final-verify.log`, `build/windows-x64/Testing/Temporary/LastTest.log`. 결과 기록은 이번 작업의 기능·수명·예비 비교 검증이며 운영 규모 성능 검증과 구분한다.

### 미완료 / 이월

BenchmarkDotNet 별도 프로세스·다회 통계, 실제 MMO 패킷 분포와 대규모 부하, Linux·원격 CI·Docker 실행은 후속이다. RIO staging copy 및 RQ/CQ 경합 최적화는 실제 병목 측정에 따라 진행한다. 기존 MMO·추가 전송·인증·스냅샷/AOI 통합 조회의 후속 범위는 유지한다. 과거 사용하지 않는 생성 산출물 삭제 거절/보존, docs Git ignore와 작업 전 untracked 상태도 변경하지 않았다.

## 후속 추가 — 2026-10-12 .NET 10 전환 및 비교 결론

> 상태: 완료 (관리 프로젝트 전환·회귀·패키징·예비 비교 범위). 위 .NET 8 및 패키지 0.2.0 기록은 당시 이력으로 보존한다. 정식 통계·운영 규모·추가 플랫폼 검증은 후속이다.

### 완료한 작업

- **신규 SDK 선택**: 루트 `global.json`에 SDK `10.0.100`, `rollForward="latestFeature"`, `allowPrerelease=false`를 설정했다. 안정 .NET 10의 최신 feature band를 선택하며 현재 선택된 SDK는 10.0.302다.
- **관리 프로젝트 전환**: `csharp/Directory.Build.props`의 target을 `net10.0`으로 변경했다. Engine·Mmo·Protocol·3개 samples·tests의 솔루션 7개 프로젝트와 별도 serialization benchmark에 함께 적용한다. 순수 관리 엔진 경계와 운영 wire Protobuf는 유지한다.
- **검증 / CI 수정**: `tools/verify_engines.py`의 관리 DLL 경로를 `net10.0`으로 변경하고, C# CI는 SDK `10.0.x`와 같은 실행 경로를 사용한다. README·계획·CODE_MAP·MMO 구조 그림 및 SDK·라우터·검증·benchmark의 4개 스킬 지침을 현재 target/버전으로 갱신했다.
- **패키지 버전**: `Helix.Engine`·`Helix.Mmo`·`Helix.Protocol`을 0.3.0으로 생성했다. `out/packages`의 기존 0.2.0 패키지는 보존한다. 0.3.0은 .NET 10 소비자를 대상으로 하며 피드에는 게시하지 않았다.
- **보고서 수정**: `tools/serialization_report.py`가 결과의 실제 Framework를 표시한다. `--baseline`은 iterations·samples·tiered compilation·OS·아키텍처·case 집합·payload bytes·encode/decode 할당량이 같은지 확인한 뒤 런타임 변화율과 데이터 기반 선택 결론을 만든다.

### 같은 조건의 관리 직렬화 재측정

Windows x64, 논리 CPU 16개, Release, tiered compilation=0, warmup 2,000회, 9 samples × 5,000 operations로 6포맷 × AOI 0/16/256/4096의 24 cases를 실행했다. .NET 10 측정 후 보존한 .NET 8 바이너리를 8.0.29에서 같은 조건으로 순차 재측정하고 .NET 10.0.10을 다시 측정했다. 아래는 마지막 .NET 10 결과 중 AOI 16의 값이다.

| 포맷 | bytes | encode µs | 전체 decode µs |
|---|---:|---:|---:|
| Protobuf | 140 | 0.467 | 0.419 |
| MessagePack | 190 | 0.152 | 0.389 |
| MemoryPack | 177 | 0.068 | 0.052 |
| JSON | 328 | 1.191 | 2.549 |
| FlatBuffers | 216 | 0.397 | 0.228 |
| fixed Binary | 176 | 0.018 | 0.018 |

새 소유 bytes 생성·전체 DTO/ID 배열 복원·mapping 비용을 포함하며 FlatBuffers 별도 verifier 비용은 제외하는 이전 비교 계약을 유지한다. runtime 8→10 변화율은 한 번씩 순차 실행한 batch median이고 교대 다회 실행이나 통계적 유의성 검증이 아니다. 모든 포맷이 개선되었다거나 관찰된 차이가 런타임만의 효과라고 주장하지 않는다.

최초 .NET 8 원시 결과와 보고서는 `out/net8-baseline/`에 보존한다. 현재 .NET 10 원시는 `out/serialization-net10.json`이며 `out/serialization-managed.json`과 동일한 복사본이다. 같은 조건의 .NET 8 재측정은 `out/serialization-net8-recheck.json`, 현재 비교·결론은 `out/serialization-report.md`다. C++ 직렬화와 IOCP/RIO 표는 앞 단계의 저장 결과를 재사용했으며 이번 .NET 전환에서 새 native 성능 측정을 한 것은 아니다.

### 비교 결론과 현재 선택

- **공통 기본 wire는 Protobuf 유지**: 양 언어 schema·기존 검증·프로토콜 계약을 유지한다. 이번 uint64 ID 분포에서 AOI 4096의 Protobuf는 24,622B, FlatBuffers는 32,856B로 Protobuf가 25.1% 작다. 예비 직렬화 속도만으로 전체 운영 포맷을 교체하지 않았다.
- **C# 전용 고빈도 경로의 후보는 MemoryPack**: AOI 16에서 Protobuf 대비 encode 6.9배·전체 decode 8.0배 빠르고, decode 관리 할당량은 224B/op 대 776B/op다. 이는 해당 payload의 quick 결과이며 MemoryPack의 C++ wire 지원을 구현한 것이 아니다. 실제 내부 경로 적용은 데이터·버전 진화 요구와 운영 부하 확인이 필요하다.
- **큰 AOI/스냅샷의 공통 binary 후보는 FlatBuffers**: AOI 4096의 전체 decode는 현재 C#에서 Protobuf 대비 21.2배, 이전 C++ 저장 결과에서 10.6배 빠르다. 큰 패킷 크기·verifier·버퍼 소유·실제 전송 비용까지 검증한 뒤 메시지별 채택을 판단한다.
- MessagePack은 범용 binary 대안, JSON은 관리·진단용 후보로 둔다. 직접 fixed Binary는 스키마 진화/필드 태그가 없는 비교 기준이며 운영 wire로 채택하지 않았다.
- **기본 IOCP와 선택 RIO 유지**: 앞 단계 loopback 처리량 차이는 작고 Python 클라이언트·staging copy·공유 큐 락의 영향을 포함한다. RIO가 항상 빠르다는 결론을 추가하지 않았다.

### 검증 상태

주 에이전트의 최종 실행 결과를 SDK 선택·프로젝트/CI/도구 소스·runtimeconfig·실행 로그·원시 결과/보고서·NuGet ZIP/nuspec 내용과 교차 확인했다. 기존 본문과 검증 결과는 수정하지 않았다.

| 검증 | 실행 / 결과 |
|---|---|
| SDK / runtime | `dotnet --version` 10.0.302. runtimeconfig target `net10.0`, 실제 benchmark Framework `.NET 10.0.10` 확인 |
| Release 빌드 | 솔루션 7개 프로젝트와 독립 benchmark 모두 빌드 성공, 각각 경고 0·오류 0 |
| 관리 계약 | `dotnet csharp/tests/Helix.Tests/bin/Release/net10.0/Helix.Tests.dll` PASS |
| 통합 회귀 | `python tools/verify_engines.py --native-backend iocp_rio` exit 0. 실제 C++ `iocp+rio`, 양쪽 echo 1,000회/churn 200회, MMO 공통 Protobuf 및 관리 계약 PASS |
| NuGet | 3개 0.3.0 pack 성공. ZIP에 `lib/net10.0` 관리 DLL·README 및 Protocol schema 확인. Protocol은 `Google.Protobuf` 3.36.2 런타임 dependency만 노출하고 `Grpc.Tools`·native payload 없음 |
| 외부 NuGet 소비자 | `build/net10-package-consumer/Consumer.csproj`이 ProjectReference 없이 3개 0.3.0 PackageReference만 사용. `dotnet restore build/net10-package-consumer/Consumer.csproj --source D:/git/helix-engine/out/packages`, `dotnet build build/net10-package-consumer/Consumer.csproj -c Release --no-restore` 성공(경고 0·오류 0). `dotnet build/net10-package-consumer/bin/Release/net10.0/Consumer.dll`을 .NET 10.0.10에서 실행하여 공개 서버 호스트 생성/Dispose·MMO QueueAdd+Tick·Protobuf encode/decode PASS |
| 관리 재측정 / 보고서 | .NET 10 및 같은 조건의 .NET 8 재측정 각각 24 cases. 마지막 net10 JSON과 managed JSON의 동일 hash 확인. baseline 비교의 설정·case·bytes·할당량 계약 검사 및 선택 결론 생성 성공 |
| 스킬 / CI 정의 | 12개 스킬 quick_validate 및 CI YAML 검증 PASS. 현재 TFM 경로 갱신 |

실행 로그는 `build/net10-verify.log`·`build/net10-benchmark.log`·`build/net8-recheck.log`에 남는다. 현재 .NET 10 테스트 실행과 예전 .NET 8 결과/패키지 이력을 구분하며, 패키지 생성은 외부 게시를 뜻하지 않는다.

### 미완료 / 이월

BenchmarkDotNet 정식 실행·통계 검증, 실제 MMO 패킷 분포/대규모 부하, Linux 런타임·원격 CI·Docker 실행은 계속 후속이다. MemoryPack/FlatBuffers 운영 경로 채택이나 공통 wire 교체를 완료로 기록하지 않는다. 기존 docs Git ignore·작업 전 untracked 상태·baseline 및 생성 산출물 삭제 거절/보존 이력은 유지한다.

## 후속 추가 — 2026-10-12 직렬화 주입 및 호환

> 상태: 완료 (이번 직렬화 주입·호환·회귀·패키징 범위). 앞 절의 Protobuf 기본 및 비교 이력은 유지하며, 이번 절은 선택 adapter의 실제 적용·호환 범위를 기록한다. 추가 플랫폼·운영 규모·adapter 성능은 후속이다.

### 완료한 작업과 책임 분리

- **신규 공통 계약**: C++ `cpp/Helix.Protocol/include/helix/protocol/messages.h` 및 C# `Helix.Protocol/Messages.cs`에 포맷 독립 Client/Server DTO를 두었다. `IClientPacketSerializer`는 요청 encode·응답 decode, `IServerPacketSerializer`는 요청 decode·응답 encode의 두 기능만 제공한다. `IPacketSerializer`가 두 역할을 조합하며, `messages::IsValid`·`MessageContract.IsValid`는 공통 의미 검증을 맡는다. DTO는 생성 타입이나 MMO 상태를 노출하지 않는다.
- **신규 조립 경계**: `SerializerRegistry`에 name·format ID·factory를 등록하고 `WireSerializer`로 포맷 메타데이터를 감싼다. 미등록 이름·중복 name/ID·factory ID 계약 위반은 실패한다. 등록은 조립 시 완료하며 실행 중 registry 수정은 지원하지 않는다. 새 adapter를 추가해도 WorldApp의 포맷별 분기나 Engine/Mmo 변경이 필요 없다.
- **샘플 주입 수정**: 양쪽 `WorldApp` 생성자는 좁은 서버 역할인 `IServerPacketSerializer`만 받는다. `[protocol] serializer`와 `HELIX_PROTOCOL__SERIALIZER`는 앱 조립 시 선택하고 연결 동안 고정한다. Engine은 바이트 프레임, Mmo는 도메인 입력·tick을 맡으며 codec 의존성을 추가하지 않았다.
- **기본 유지 / 기존 API 보존**: ID 1 Protobuf는 기존 raw payload를 그대로 사용한다. 기존 generated 타입·`PacketCodec`/C++ Codec API를 유지하여 이전 Protobuf 클라이언트와 API 소비자가 동작한다.
- **선택 FlatBuffers**: C++ `Helix.Protocol.FlatBuffers`·C# `Helix.Protocol.FlatBuffers`, ID 2 및 운영 정본 `protos/helix_mmo.fbs`를 추가했다. 양 언어는 네트워크 입력을 verifier로 검사한 뒤 소유 DTO/ID 배열로 복원한다. schema identifier는 `HXMP`, flatc/runtime은 25.2.10이다.
- **선택 MemoryPack**: C# `Helix.Protocol.MemoryPack`, ID 3을 추가했다. `MemoryPacket`은 VersionTolerant 및 영구 order 0..13을 사용한다. 읽기 전 member 길이 합·고정 폭·배열 count를 실제 bytes와 대조하고 trailing bytes를 거부한다. bounded 배열 formatter가 할당 전 count를 확인한다. little-endian C# 전용이며 C++에서는 지원하지 않는다.
- 선택 포맷은 `HXS1` 8바이트 envelope를 사용한다: magic 4바이트, format ID, revision 1, direction(Client=1/Server=2), reserved 0. codec은 포맷·revision·방향·예약 필드를 검사하고 잘못된 payload를 다른 포맷으로 추측하지 않는다. 기존 4바이트 big-endian TCP 길이는 envelope를 포함한 전체 payload 길이다.
- **빌드 / 생성**: 기본 C++ 선택 adapter를 `HELIX_BUILD_FLATBUFFERS=OFF`로 제외할 수 있고 Protocol OFF는 둘 다 제외한다. 설치 소비자는 `helix::protocol_flatbuffers`를 링크한다. 공식 flatc 25.2.10을 고정 upstream source에서 자동 빌드하며 C# generated 소스와 재생성 결과의 일치를 확인했다.
- **패키징**: 관리 Engine·Mmo·Protocol 및 선택 FlatBuffers·MemoryPack의 5개 NuGet을 0.4.0으로 생성했다. 이전 0.3.0 패키지는 보존하며 외부에 게시하지 않았다. 선택 패키지의 dependency는 해당 serializer 모듈에 한정한다.
- **문서 / 스킬**: README·계획·CODE_MAP·MMO·CONFIGURATION 및 [SERIALIZATION.md](../SERIALIZATION.md)에 주입 구조·설정·wire·스키마 규칙·지원 한계를 기록했다. 프로젝트 스킬 12개와 라우팅 지침을 현재 경계로 갱신했다.

### 호환 계약과 한계

| 포맷 | C++ | C# | wire |
|---|---|---|---|
| Protobuf ID 1 | 지원 | 지원 | 기존 raw Protobuf |
| FlatBuffers ID 2 | 지원 | 지원 | HXS1 + 검증된 FlatBuffers |
| MemoryPack ID 3 | 미지원 | little-endian 지원 | HXS1 + VersionTolerant MemoryPack |

서로 다른 포맷은 byte 호환이 아니다. 양쪽이 같은 format·envelope revision·메시지 버전을 사용해야 한다. 디코딩한 DTO를 다른 adapter로 다시 인코딩할 수는 있지만 자동 협상·연결 중 포맷 변경·자동 transcoding은 구현하지 않았다.

Protobuf 필드 번호/타입, FlatBuffers 기존 필드/union 순서·숫자, MemoryPack 영구 order/타입을 보존하고 새 항목은 끝에 추가한다. 이번 검증은 실제 추가 필드 fixtures를 읽는 범위이며 모든 과거/미래 schema의 무조건 호환을 보장하지 않는다. DTO는 unknown field를 보관하지 않아 byte-preserving proxy forwarding 계약이 아니다.

request ID는 응답 상관관계이며 인증·중복 실행 방지가 아니다. QUEUED는 tick 적용 전 접수다. 포맷 식별자와 verifier는 암호화나 운영 프로토콜 인증을 제공하지 않는다. 기존 snapshot/AOI 조회 경계와 MMO 후속 범위는 유지한다.

### 검증 상태

주 에이전트의 실행 결과를 실제 DTO/serializer/factory·설정·생성 schema·수명/호환 테스트·CMake cache·최종 로그·외부 소비자 소스·패키지 산출물과 교차 확인했다.

| 검증 | 실행 / 결과 |
|---|---|
| 관리 Release | 솔루션 9개 프로젝트 빌드 성공, 경고 0·오류 0. `build/serializers-managed-build.log` |
| 관리 계약 / malformed | 3개 codec, 공통 의미 검증, envelope/format/direction 불일치, 2,000개 malformed corpus, 추가 MemoryPack member/FlatBuffers field, 사용자 factory 및 기존 회귀 PASS. `build/serializers-managed-test.log` |
| native 계약 | 마지막 native 빌드 exit 0 및 CTest 5/5 PASS: 부분 송신·실제 IOCP/RIO·Protobuf·packet-serializers·MMO. `build/serializers-native-build.log`·`build/serializers-ctest.log` |
| 공유 포맷 fixtures | C++→C#·C#→C++ 각각 Client 3개 + Server 9개 cases를 Protobuf/FlatBuffers 2포맷으로 실행 PASS. 모든 DTO 필드·uint64 최대값·int32 최소값·좌표·상태·AOI 0/16/256/4096과 추가 FlatBuffers 필드 검증 |
| 실제 MMO / 회귀 | 역할별 인터페이스 분리 후 최종 `verify_engines.py` 전체 실행 exit 0. 공유 포맷 양방향 fixtures와 C++ Protobuf/FlatBuffers·C# Protobuf/FlatBuffers/MemoryPack의 5개 서버 조합에 이동·전투·AOI·request ID·잘못된 입력·분할 프레임 PASS. 기존 raw Python Protobuf 클라이언트 및 echo/churn PASS. `build/serializers-verify.log` |
| 외부 SDK 소비자 | 역할 인터페이스 분리 후 관리 9개 Release 경고 0·오류 0 및 관리 tests, native 빌드·CTest 5/5, 재설치한 MSVC 외부 CMake `find_package(helix)` 소비자 PASS. 0.4.0 재pack 후 새 `build/nuget-serializer-final` 캐시에 로컬 패키지와 기존 공식 dependency feed로 복원한 ProjectReference 없는 외부 NuGet 소비자 빌드·실행 PASS. 소비자는 공개 `IClientPacketSerializer`·`IServerPacketSerializer`를 실제 사용하여 3개 codec의 요청 encode/decode를 확인 |
| 생성 / 패키지 / 스킬 / CI 정의 | 공식 flatc source 빌드·관리 생성 소스 일치 PASS. 최종 README를 반영한 5개 pack 및 artifact 검증, 12개 스킬 validator·상대 링크·CI YAML 모두 exit 0 |
| 선택 OFF→ON | 초기 구성 전환 시 테스트 실행파일 이름 충돌을 발견하여 OFF 타깃을 `helix_protobuf_serializer_test`로 분리. 해당 타깃 빌드·실행 exit 0 후 ON 재구성 exit 0 및 전체 회귀 PASS. 최종 cache는 `HELIX_BUILD_FLATBUFFERS=ON`, `HELIX_BUILD_BENCHMARKS=OFF` |

### 미완료 / 이월

native OFF→ON 및 최종 전체 회귀 검증은 완료했다. Linux·원격 CI·Docker·운영 규모와 새 adapter의 성능은 미검증이다. 이전 bare-state benchmark 배율은 이번 envelope·verifier·VersionTolerant·mapping을 포함한 adapter의 성능 수치로 사용하지 않는다. 자동 협상·중간 포맷 전환·unknown-field byte-preserving DTO forwarding은 후속이다. 과거 생성 산출물 삭제 거절/보존, docs Git ignore, 작업 전부터 untracked였던 소스 이력은 유지한다.

## 후속 추가 — 2026-10-12 실제 적용 codec의 헤더·검증 포함 벤치마크

> 상태: 완료 (현재 adapter의 메모리 내 frame 생성·검증·전체 복원 측정 범위). 이전 bare-state 결과와 미측정 당시 기록은 보존한다. 실제 네트워크·운영 부하·다른 플랫폼·더 긴 다회 launch 검증은 후속이다.

### 완료한 작업과 측정 계약

- **신규 독립 관리 benchmark**: `csharp/benchmarks/Helix.WireBenchmarks`는 실제 `SerializerRegistry`·`WireSerializer`·Protobuf/FlatBuffers/MemoryPack adapter를 호출한다. `WireCase`의 encode는 새 소유 frame을 생성하고 decode는 완결된 입력 frame에서 전체 DTO/배열을 복원·검증한다. 운영 serializer 구현을 복사·간소화하지 않았다.
- **신규 선택 C++ benchmark**: `cpp/benchmarks/wire_serialization.cpp`는 실제 adapter와 `src/proto/FrameCodec.h`의 길이 header를 사용한다. `helix_wire_benchmark`는 계측 없는 시간 실행파일이며 `helix_wire_allocation_probe`만 `CountNew.cpp`에 링크한다. 도메인 및 운영 serializer 구현은 변경하지 않았다.
- 메시지는 요청 Move/Attack/Look, 응답 Welcome/Queued Reply와 State AOI 0/16/256/4096의 총 9개다. 동일 request/엔티티 ID, 좌표, HP, tick, AOI 배열을 사용하고 각 case의 모든 필드를 왕복 대조한다.
- **encode 포함 범위**: DTO mapping·format별 bytes·선택 포맷의 8B HXS1 envelope·4B big-endian TCP length·새 소유 frame 복사. 전체 wire bytes는 모든 header를 포함한다. 기본 raw Protobuf는 8B envelope가 없다.
- **decode 포함 범위**: 완결 frame의 length/최대 1MiB 검사·format/revision/direction/reserved 검사·파싱·FlatBuffers verifier 또는 MemoryPack member-length/count 검사·전체 소유 DTO/배열 복원·공통 의미 검증.
- **제외 범위**: socket I/O·세션 송수신 큐·async Stream 수신 buffer·FrameReader의 분할/누적·암호화·tick. decode 입력 frame은 setup에서 만들어 재사용하며, 수신 계층의 buffer 할당을 이 표의 alloc으로 해석하지 않는다.
- **도구 / 문서 추가**: `tools/wire_native_benchmark.py`와 `tools/wire_benchmark_report.py`가 순차 측정·원시 결과 집계·완전한 case 및 안정 할당 검사·실제 값과 결론을 생성한다. README·SERIALIZATION·CODE_MAP, `helix-benchmark` 스킬 및 CI smoke 정의를 갱신했다.

### 실행 환경과 계측

C#은 Windows 11 x64, AMD Ryzen 7 7700(8 physical/16 logical cores), SDK 10.0.302, runtime .NET 10.0.10, Release에서 **BenchmarkDotNet 0.15.8을 실제 실행**했다. 3포맷 × 9메시지 × 2연산의 54조건, case별 launch 1회·warmup 3회·measurement 9회 × 100ms 목표·tiered compilation=0·MemoryDiagnoser다. outlier 처리 후 실제 표본은 7~9개이며 평균/error/표준편차·측정 원문을 `out/wire-bdn`에 보존한다. 짧은 단일 launch 결과를 더 긴 운영 환경 검증으로 일반화하지 않는다.

C++은 MSVC 194435213 x64 Release로 3개 독립 프로세스를 순차 실행했다. warmup 2,000회, 실행마다 9 batches × 10,000 operations, 포맷 순서 교대이며 각 실행 batch median 3개의 중앙값과 run min–max를 보고한다. 별도 allocation probe는 3회 × 1,000 operations로 `new`/`new[]` 요청 bytes/calls를 집계한다(정렬/nothrow 포함). 시간 실행파일에는 allocation hook이 없으며, probe는 direct malloc·OS/외부 runtime 할당 및 peak/RSS를 측정하지 않는다. C# managed B/op와 native requested-new 지표는 구분한다.

### 현재 adapter의 실제 결과

아래 C# 값은 BDN 평균이다. error와 전체 54조건은 [wire-report.md](../../out/wire-report.md) 및 [wire-managed.json](../../out/wire-managed.json)에 있다.

| 메시지 | 포맷 | 전체 bytes | encode µs | validated decode µs | encode alloc B/op | decode alloc B/op |
|---|---|---:|---:|---:|---:|---:|
| Move | Protobuf | 39 | 0.062 | 0.080 | 320 | 232 |
| Move | FlatBuffers | 116 | 0.266 | 0.288 | 1,024 | 512 |
| Move | MemoryPack | 106 | 0.152 | 0.140 | 488 | 232 |
| State4096 | Protobuf | 24,634 | 59.721 | 35.229 | 82,368 | 98,832 |
| State4096 | FlatBuffers | 32,924 | 31.252 | 136.400 | 296,128 | 131,976 |
| State4096 | MemoryPack | 32,878 | 4.152 | 4.909 | 98,808 | 33,032 |

C++ State4096의 시간은 3 runs의 중앙값이며 allocation은 별도 probe의 안정값이다. 원문은 [wire-native.json](../../out/wire-native.json)·`out/wire-native-runs/`에 있다.

| 포맷 | encode µs | validated decode µs | encode requested new B / calls | decode requested new B / calls |
|---|---:|---:|---:|---:|
| Protobuf | 19.616 | 15.468 | 180,512 / 16 | 131,207 / 5 |
| FlatBuffers | 4.394 | 0.988 | 100,746 / 4 | 32,807 / 1 |

### 선택 결론 보정

- **공통 기본 및 작은 메시지는 Protobuf 유지**: 이번 C# 조건에서 Move/Attack/Look/Welcome/Reply/State0의 encode·validated decode·wire 크기가 모두 유리했다. 예를 들어 Look은 전체 Protobuf 10B, FlatBuffers 68B, MemoryPack 106B다.
- **C# 큰 상태의 MemoryPack 후보**: State4096은 Protobuf 대비 encode 14.38배·decode 7.18배 빠르지만 encode 관리 할당이 약 20% 늘고 wire도 약 33% 커진다. decode 할당은 줄어든다. 모든 크기·비용에서 우위라고 기록하지 않는다.
- **C++ 큰 상태의 FlatBuffers 이점**: State4096에서 현재 C++ encode·decode 시간과 requested-new bytes/calls가 줄었다. 반면 현재 C# 전체 복원은 Protobuf보다 3.87배 느리며 할당도 크다. 같은 schema의 포맷 이름으로 양 언어 성능을 동일하게 판단하지 않는다.
- **현재 C++→C# 큰 snapshot에는 FlatBuffers 권장 보류**: C++ encode + C# validated decode의 산술 참고값은 Protobuf 54.845µs, FlatBuffers 140.793µs다. 서로 다른 측정의 median/mean을 더한 값이며 실제 네트워크 latency나 동시 실행 비용이 아니다. C# adapter 전체 복원 경로의 최적화·재측정 후 판단한다.
- 이전 bare-state의 FlatBuffers/MemoryPack 배율을 새 envelope·verifier·VersionTolerant adapter의 성능으로 사용하지 않는다. 이번에는 운영 codec과 schema를 최적화하거나 기본 wire를 변경하지 않고 현재 경로를 측정했다. 포맷은 연결 동안 고정되는 기존 계약을 유지한다.

### 검증 상태

실제 benchmark·frame/검증 호출·allocation 링크 분리·실행 로그·BDN 원문·원시 JSON/CSV·생성 보고서 및 최종 CMake cache를 교차 확인했다.

| 검증 | 실행 / 결과 |
|---|---|
| 관리 빌드 / smoke | 독립 `Helix.WireBenchmarks` Release 빌드 경고 0·오류 0, 실제 27개 framed/validated round-trip PASS. `build/wire-managed-build.log` |
| 관리 BDN | 54조건 실행 성공 및 raw export, `build/wire-managed-measure.log` exit 0. `out/wire-managed.json`·`out/wire-bdn` 보존 |
| native 빌드 / 측정 | Release timing·allocation probe 빌드와 3 runs × 각 36연산 조건 성공. `build/wire-native-build.log`·`build/wire-native-measure.log` exit 0 |
| 결과 완전성 | 관리 54조건·native 각 36조건, encode/decode wire bytes 일치 및 3회 native allocation 안정값 검사 PASS. `out/wire-report.md` 생성 |
| 도구 / 정의 | 스킬 validator·Python py_compile·CI YAML·git diff check PASS. CI에는 독립 wire benchmark smoke를 연결 |
| 최종 선택 cache | benchmark OFF 복원 구성 exit 0. `HELIX_BUILD_BENCHMARKS=OFF`, 선택 FlatBuffers ON 유지 |

### 미완료 / 이월

Linux·원격 CI·실제 네트워크 부하·운영 MMO 전체 지연, 새로운 adapter 최적화 및 더 긴 다회 launch는 미실행이다. 관리 BDN 실행은 이번 메모리 내 wire 경로에 한정하며 이전 bare-state의 정식 측정을 새로 수행한 것이 아니다. 자동 협상·포맷 전환·forwarding 및 기존 MMO/전송의 후속 범위는 유지한다. docs Git ignore·과거 생성 산출물 보존·작업 전 untracked 이력과 기존 Phase 번호도 유지한다.

## 후속 추가 — 2026-11-03 C++ 작업 그래프 병렬 MMO 실험

> 상태: 완료 (C++ 선택 실험 모듈·정확성·Windows 로컬 통신·기준 측정 범위). 운영 규모 확장성과 병렬 성능 향상은 확인하지 않았다. 기존 Phase 번호와 이전 기록은 보존한다.

### 목표와 완료한 작업

사용자 요청에 따라 한 전장 내부의 이동·공격 판정·서로 다른 대상의 HP 반영을 여러 worker가 실행하는 구성을 구현했다. C++과 C#을 독립적으로 진행한다는 경계를 유지하며 이번 작업에서 C# 구현·빌드·검증을 확대하지 않았다.

- **신규 선택 모듈**: `HELIX_BUILD_PARALLEL_MMO=ON`으로 `helix::mmo_parallel`을 빌드한다. `parallel_world.h`의 `ParallelWorldInstance`는 기존 순차 `WorldInstance`를 대체하지 않는 별도 실험 API다. `ParallelWorld.cpp`와 `SpatialIndex.h`에 엔티티 배치 갱신·피격 예산·거리 기반 공간 조회를 구현했다.
- **신규 실행기 경계**: `world_executor.h`의 `IWorldExecutor::Execute(span<WorldTask>)`에 작업과 선행 의존성만 노출한다. `WorldExecutor.cpp`에 순차·Taskflow adapter를 제공하고, 여러 월드에 같은 실행기를 주입해 프로세스의 worker pool을 공유한다. Taskflow 4.1.0 commit `45366fe5bc4f2f8ec9aa590b40c504e296886865`를 고정하며 외부 소비자에게 Taskflow 헤더를 요구하지 않는다.
- **기존 샘플·설정 수정**: `[mmo]`의 `execution`, `worker_threads`, `batch_size`, `cell_size`, `visibility_radius`, `max_hits_per_tick`과 `HELIX_MMO__*` 환경변수를 조립·검증한다. 기본은 기존 `sequential`이며 선택 모듈 없이 `parallel`을 요청하면 기동을 거부한다. 샘플의 Field·Arena tick 호출은 계속 순차이며 각 월드 안의 작업이 병렬 실행된다.
- **신규 검증·측정**: `parallel_world_test.cpp`, `parallel_benchmark.cpp`, `tools/parallel_world_benchmark.py`, `tools/verify_parallel_world.py` 및 선택 모듈 CI 정의를 추가했다. README·계획·설정·MMO·코드 지도와 [PARALLEL_MMO.md](../PARALLEL_MMO.md)에 실제 실행 계약과 미구현 범위를 기록했다.
- **라이브러리 조사·스킬 유지**: Taskflow·oneTBB·enkiTS·Marl의 공식 자료를 확인하고 첫 adapter로 Taskflow를 선택했다. Marl은 공식 저장소의 archive 상태를 명시했다. 라이브러리 간 성능 비교는 수행하지 않았다. 관련 프로젝트 스킬 6개를 이번 C++ 독립 범위·병렬 writer·검증 계약에 맞춰 수정하고 별도 설치 복제본 유무를 검사했다. 별도 복제본은 존재하지 않았다.

### 설계 결정과 콘텐츠 의미

실행 순서는 입력·정책 포착 → 추가/제거 → 엔티티별 이동 → 공간 인덱스·피격 계획 → 피격 판정 → 대상별 분류 → 대상 배치별 HP 반영 → 완료 상태 게시다. 이동 작업은 서로 다른 엔티티 슬롯을, HP 작업은 서로 다른 대상 슬롯을 동시에 쓴다. `atomic_flag`로 같은 월드의 동시 tick과 같은 스레드의 tick 재진입을 모두 거부하지만 상태 writer를 한 worker로 제한하는 구조는 아니다. 공간 인덱스 구축·계획·분류 등 직렬 단계는 남아 있다.

새 단계별 규칙은 기존 명령별 순차 처리와 의미가 다르다. 정확성·비용 비교의 기준은 **같은 `ParallelWorldInstance`를 순차 실행기와 병렬 실행기로 실행한 결과**다. 같은 대상의 피해는 정해진 입력 순서로 반영하고, 판정 시점에 살아 있던 공격자는 같은 tick에 죽어도 공격하므로 상호 처치를 허용한다. 광역 공격의 hit 예산을 초과하면 명령 전체를 거부한다. 회복·버프·흡혈·반사·이동 충돌은 이번 규칙에 없다.

이동·전투 정책은 순수·결정적·동시 호출 안전이어야 한다. 정책 교체는 다음 tick 시작 경계에 포착한다. 기반 작업 실패는 소비한 입력을 자동 재시도하지 않고 예외를 전파하며 이전 게시 상태를 유지한다. executor는 예외 전파 전에 시작된 작업을 회수해야 한다. 상태·정책·공간 인덱스를 같은 완료 객체로 원자 게시하고 조회자는 마지막 완료 상태를 읽는다. AOI 질의와 응답 직렬화는 여전히 호출 스레드에서 실행한다.

### 검증 상태

신규 파일이 untracked인 상태도 포함해 실제 공개 헤더·실행기·월드·테스트·문서·측정 산출물을 확인하고, 주 에이전트의 실행 결과를 다음과 같이 기록했다.

| 검증 | 실행 / 결과 |
|---|---|
| 루트 C++ Release | `cmake --preset windows-x64 -DHELIX_BUILD_PARALLEL_MMO=ON` 구성 및 전체 Release 빌드 exit 0. CTest 6/6 PASS(`windows-iocp-rio`, 기존 MMO·serializer 회귀, 신규 `parallel-world` 포함) |
| 독립 C++ 선택 ON | `cpp` 정본에서 Protocol OFF·Parallel MMO ON 구성·Release 빌드 exit 0. `build/parallel-core` CTest 4/4 PASS |
| 병렬 월드 계약 | worker 1/2/4/8의 상태·명령 결과 일치, 같은 월드 이동 정책의 실제 동시 실행, 공유 실행기 다중 월드와 동시 snapshot/AOI reader, 동시 tick·같은 스레드 재진입 거부, 정책 교체 경계, 상호 처치·광역 예산, 바운드 입력·잘못된 설정·예외 후 미게시 PASS. 공간 질의는 별도의 전체 거리 검사와 대조 PASS |
| Windows 실제 통신 | `python tools/verify_parallel_world.py --backend iocp_rio` exit 0. Python Protobuf 클라이언트가 C++ 기존 순차·병렬 서버 모두에서 이동·전투·AOI와 잘못된 입력을 확인. C# 빌드 없이 수행 |
| 설치 SDK 외부 소비자 | 독립 SDK를 `build/parallel-installed`에 설치하고 외부 CMake 소비자 빌드·`build/parallel-consumer-build/Release/consumer.exe` 실행 PASS. Taskflow 헤더 없이 공개 병렬 월드·실행기를 사용 |
| 선택 OFF | Protocol OFF·Parallel MMO OFF 독립 구성 및 기존 world test 타깃 빌드 exit 0. 해당 구성의 `world-instance` CTest PASS |
| 측정 / 도구 / 정의 | 최종 바이너리에 `python tools/parallel_world_benchmark.py --out out/parallel-world-final` 실행 exit 0. 기본 3회·45개 시나리오/실행기/run 조합 결과 비교 PASS. Python 문법 검사, 변경 스킬 validator 및 `build/validate-parallel-changes.py`의 UTF-8·상대 링크·YAML·별도 설치 복제본 유무 검사 PASS. CI 정의 추가는 원격 CI 실행 증거가 아님 |

동일 스레드 tick 재진입 검사 추가와 `atomic_flag` 수정 후 루트 CTest 6/6, 독립 C++ CTest 4/4, IOCP/RIO 두 native 모드 통신, 재설치 SDK 외부 소비자를 모두 다시 검증해 PASS했다. Windows 프로세스 강제 종료를 사용하는 통신 도구의 정리는 graceful shutdown 검증으로 간주하지 않는다. 짧은 기능 smoke와 아래 반복 측정 결과도 구분한다.

### 기준 측정 결과와 해석

Windows 11 x64 Release의 최종 바이너리에서 분산 전투·한 지점 밀집·보스 집중 공격의 3개 시나리오를 엔티티 4,096개, warmup 10 ticks, 측정 100 ticks, 3 runs로 실행했다. inline 및 Taskflow worker 1/2/4/8에 같은 입력을 사용했다. 배치 128·셀 크기 4·가시거리 10·광역 반경 0.15·tick hit 예산 131,072를 사용하며 최종 조건과 원시 결과는 `out/parallel-world-final/results.json`·`raw.csv`·`report.md`에 보존한다. 앞선 `out/parallel-world`는 초기 측정 산출물로 유지한다.

| 시나리오 | inline tick p99 µs | Taskflow 4 workers tick p99 µs |
|---|---:|---:|
| 분산 전투 | 2,128.5 | 2,387.7 |
| 한 지점 밀집 | 3,276.2 | 3,363.9 |
| 보스 집중 공격 | 1,107.5 | 1,261.4 |

표는 run별 p99의 중앙값이며 전체 표본을 합친 p99가 아니다. 모든 worker·run에서 최종 상태 hash, 적용/거부 수, visible ID 수가 일치했다. tick 시간에는 복사·공간 인덱스·그래프 구축/검증·판정·분류·HP 반영·게시를 포함하고 입력 enqueue·네트워크·외부 I/O·별도 AOI 질의는 제외한다.

**이번 단순 규칙에서는 4 workers가 inline보다 빨라지지 않았다.** 실제 상태의 동시 갱신과 결과 일치는 검증했지만 처리량 향상·코어 비례 확장·실제 MMO 수용 인원을 입증하지 않는다. 직렬 단계와 작업 배치 비용은 존재하며 단계별 비용을 아직 계측하지 않았으므로 특정 구간을 실측 병목으로 단정하지 않는다.

### 미완료 / 이월

Linux 런타임·원격 CI·TSan, 실제 MMO 콘텐츠·네트워크 부하와 장시간 측정, 단계별 시간·할당 계측, AOI·패킷 생성의 병렬 작업화, 회복·버프·반사 등 충돌 규칙, 고정 deadline tick 스케줄러는 후속이다. 기존 C# 최적화·추가 전송·플랫폼 이월은 그대로 유지한다. Redis 안정화나 Kafka 신규 연동은 이번 작업에 포함하지 않았다. 새로운 Phase를 자동 생성하지 않고 이번 C++ 실험을 Phase 10의 날짜별 후속으로 기록했다.

## 후속 추가 — 2026-11-04 현재 구현 기준 문서 정리

> 상태: 완료 (현재 안내와 과거 설계 자료의 구분·공개 API·빌드/사용 경로 정리 범위). 코드 동작 변경과 C# 구현·빌드는 이번 문서 작업에 포함하지 않았다.

- README에 C++ 선택 병렬 월드의 빌드·실행·측정 절차와 성능 향상 미확인·AOI/직렬화의 현재 한계를 반영했다. `docs/API.md`는 실제 공개 선언의 `SessionCloseReason`·`TransportType`, 선택 C ABI v2 및 독립 C# API로 맞췄다.
- `docs/BUILD.md`·`docs/USAGE.md`의 과거 경로와 P/Invoke 사용 안내를 현재 독립 빌드·샘플·설치 SDK 사용법으로 교체했다. `docs/CONFIGURATION.md`의 MMO·RIO·epoll 설정을 실제 소비 경로와 맞췄다.
- 문서 목차·계획의 현재 안내와 초기 설계 이력을 구분했다. `docs/CODE_MAP.md`의 버전을 정정하고 CODE_MAP·SERIALIZATION에서 Git에 없는 `build/`·`out/` 생성물을 필수 탐색 링크로 사용하지 않도록 정리했다. `helix-code-map` 스킬에 실제 선언 대조·초기 자료 보존·생성 산출물 링크 처리 지침을 보완했다. 이전 Phase 이력과 병렬 실험의 수치·미완료 범위는 유지한다.

검증은 이번 턴 주 에이전트의 실행 결과를 기준으로, 루트 Release CTest 재실행 6/6 PASS, 관련 스킬 6개 validator·별도 설치 복제본 유무·YAML 검사 PASS, `git diff --check` PASS를 확인했다. `python -X utf8 build/validate-current-docs.py`로 현재 안내 11개(README·계획·문서 목차·API·USAGE·BUILD·CONFIGURATION·MMO·PARALLEL_MMO·CODE_MAP·SERIALIZATION)의 UTF-8·코드 펜스·상대 링크·dotnet project 경로·오래된 API 이름 검사도 PASS했다. 문서 정리를 위해 기존 빌드·통신·벤치마크의 검증 범위를 확대하거나 Linux·원격 CI·운영 수용 인원을 완료로 처리하지 않았다.
