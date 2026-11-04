# 빌드와 검증

> 갱신: 2026-11-04. 모든 명령은 저장소 루트 기준이다. 과거 검증 이력은 [Phase 기록](phases/)에 보존한다.

## 요구 환경과 독립 빌드

C++은 CMake 3.21 이상·C++20 컴파일러가 필요하다. Windows 정본은 Visual Studio 2022 C++ 도구와 루트 프리셋이다. C#은 .NET 10 SDK, Python 검증 도구는 Python 3.10 이상을 사용한다. C++과 C#은 서로 빌드하거나 네이티브 DLL을 공유하지 않는다.

| 구성 | 기본값 | 의존성과 출력 |
|---|---|---|
| C++ Engine·기존 MMO | ON | 정적 `helix::helix`, `helix::mmo` |
| `HELIX_BUILD_PROTOCOL` | ON | 고정 Protobuf 36.2/protoc 복원, MMO 네트워크 샘플 |
| `HELIX_BUILD_FLATBUFFERS` | ON | Protocol 사용 시 고정 FlatBuffers adapter |
| `HELIX_BUILD_CAPI` | OFF | C ABI 공유 라이브러리, C# 실행 경로와 별개 |
| `HELIX_BUILD_BENCHMARKS` | OFF | native 직렬화 benchmark |
| `HELIX_BUILD_PARALLEL_MMO` | OFF | Taskflow 4.1.0 복원, `helix::mmo_parallel`·테스트·도메인 benchmark |

첫 복원에는 네트워크 연결이 필요하다. KCP 구현 소스는 저장소에 포함되어 있다.

## Windows C++

```powershell
cmake --preset windows-x64
cmake --build --preset windows-release --parallel 8
ctest --test-dir build/windows-x64 -C Release --output-on-failure
```

Debug는 `windows-debug` 빌드 프리셋과 `ctest -C Debug`를 사용한다. 서버 출력은 `build/windows-x64/<구성>/`에 생성된다.

Visual Studio에서는 `build/windows-x64/helix-engine.sln`을 연다. 프로젝트는 `01 Engine`, `02 Modules`, `03 Samples`, `04 Tests`, `05 Benchmarks`, `90 Dependencies`, `99 CMake` 폴더로 나뉜다. 선택한 빌드 옵션에 해당하는 타깃이 있는 폴더만 생성한다. 외부 라이브러리는 Dependencies 아래에 묶이며, CMake로 재생성해도 분류가 유지된다. 이 분류는 타깃 표시만 바꾸며 빌드·링크 관계는 유지한다.

C++만 별도 디렉터리에서 구성할 때:

```powershell
cmake -S cpp -B build/cpp -G "Visual Studio 17 2022" -A x64
cmake --build build/cpp --config Release --parallel 8
ctest --test-dir build/cpp -C Release --output-on-failure
```

기존 코어/오프라인 MMO만 필요하면 새 디렉터리에 `-DHELIX_BUILD_PROTOCOL=OFF -DHELIX_BUILD_PARALLEL_MMO=OFF`를 지정한다. 이미 구성한 디렉터리의 선택 값은 CMake cache에 남으므로 필요한 ON/OFF 값을 명시한다.

## Linux C++

```sh
cmake -S cpp -B build/cpp-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/cpp-linux --parallel 2
ctest --test-dir build/cpp-linux --output-on-failure
```

Linux TCP는 epoll이며 Windows RIO는 사용할 수 없다. 선택 병렬 월드의 Windows/Linux CI 정의는 `.github/workflows/parallel-mmo-ci.yml`에 있다. 정의 추가와 실제 CI 성공은 별도다. 2026-11-03 병렬 실험의 로컬 Linux 실행은 미검증이다.

## 순수 C#

```powershell
dotnet build csharp/Helix.sln -c Release -m:1
dotnet run --project csharp/samples/Helix.Echo -c Release -- config/helix.toml
dotnet run --project csharp/tests/Helix.Tests -c Release
```

`helix.dll`이나 C++ 빌드 출력이 필요하지 않다. C# 테스트는 실행형 테스트 프로젝트다. 선택 직렬화 모듈은 [예제 메시지 계약](SERIALIZATION.md), 별도 benchmark 명령은 [성능 측정 가이드](BENCHMARKS.md)를 따른다. C++ 병렬 변경만 검증할 때 이 관리 빌드를 수행할 필요는 없다.

## 실제 통신과 성능

TCP echo 서버를 실행한 뒤 다른 터미널에서:

```powershell
python tools/echo_bot.py --port 5000 --clients 200 --messages 50 --size 256
python tools/echo_bot.py churn --port 5000 --clients 500 --rounds 20
```

MMO Python 도구 준비와 C++ 두 실행 모드 검증:

```powershell
python -m pip install --target build/protobuf-python -r tools/requirements-protobuf.txt
python tools/generate_protocol.py
python tools/verify_parallel_world.py --backend iocp_rio
# 빌드·다른 검증이 끝난 뒤 순차 측정
python tools/parallel_world_benchmark.py
```

`verify_parallel_world.py`는 로컬 C++ 순차·병렬 서버를 생성·종료하며 C#은 빌드하지 않는다. 기본 실행 파일 경로는 Windows Release다. 다른 빌드 디렉터리/플랫폼은 `--exe`, Linux는 `--backend epoll`을 전달한다. Python 생성에는 protoc 36.2가 필요하며 독립 빌드 경로는 `generate_protocol.py --protoc <경로>` 또는 `HELIX_PROTOC`로 지정한다.

두 엔진 출력을 모두 검증할 환경에서는 `python tools/verify_engines.py`를 사용한다. 통신 도구의 프로세스 강제 종료는 graceful shutdown 검증이 아니다. benchmark의 로컬 결과·실행 조건은 `out/`에 생성하며 Git에는 포함하지 않는다.

## 설치 소비자 검증

```powershell
cmake --install build/cpp --config Release --prefix D:/sdk/helix
```

외부 프로젝트에서 `find_package(helix CONFIG REQUIRED)`와 설치된 `helix::helix`·`helix::mmo`·선택 `helix::mmo_parallel`을 사용한다. 소비자 구성에 `-DCMAKE_PREFIX_PATH=D:/sdk/helix`를 전달한다. Protocol ON 설치물은 Protobuf 36.2.0 EXACT CMake 패키지를 찾는다. Protocol OFF SDK로 도메인 소비자를 검증하면 Protobuf 복원이 필요하지 않으며, 병렬 소비자에게 Taskflow 헤더를 노출하지 않는다.

## 선택 C++ 병렬 월드

```powershell
cmake --preset windows-x64 -DHELIX_BUILD_PARALLEL_MMO=ON
cmake --build --preset windows-release --parallel 8
ctest --test-dir build/windows-x64 -C Release --output-on-failure
$env:HELIX_MMO__EXECUTION="parallel"
./build/windows-x64/Release/helix_world_server.exe config/helix.toml
# 서버 종료 후
Remove-Item Env:HELIX_MMO__EXECUTION
```

프로토콜 없이 도메인만 검증할 수도 있다.

```powershell
cmake -S cpp -B build/parallel -DHELIX_BUILD_PROTOCOL=OFF -DHELIX_BUILD_PARALLEL_MMO=ON
cmake --build build/parallel --config Release --parallel 8
ctest --test-dir build/parallel -C Release --output-on-failure
```

이 구성에도 Taskflow 복원은 필요하다. 네트워크 샘플 `helix_world_server`는 Protocol ON에서 생성된다. 테스트와 benchmark는 [단계별 규칙과 측정 경계](PARALLEL_MMO.md)를 따른다.
