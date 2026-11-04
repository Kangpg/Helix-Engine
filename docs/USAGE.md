# 사용 가이드

> 갱신: 2026-11-04. 모든 명령은 저장소 루트 기준이다. C++과 순수 C#은 독립 엔진이다.

[빌드·검증](BUILD.md) → [설정](CONFIGURATION.md) → [API](API.md) 순서로 기본 서버를 작성한다. 앱이 받은 메시지의 형식과 처리 로직을 정의하며, 별도 콘텐츠 모듈을 사용할 수도 있다.

## C++ 서버 작성과 실행

C++20 컴파일러와 CMake 3.21 이상이 필요하다. Windows 정본 구성은 Visual Studio 2022 프리셋이다.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-release --parallel 8
ctest --test-dir build/windows-x64 -C Release --output-on-failure
./build/windows-x64/Release/helix_echo_phase1.exe
```

기본 구성은 Protobuf·FlatBuffers를 복원한다. 코어와 기존 오프라인 MMO만 쓰려면 새 빌드 디렉터리에서 `HELIX_BUILD_PROTOCOL=OFF`, `HELIX_BUILD_PARALLEL_MMO=OFF`로 구성한다. KCP 원본 소스는 `ThirdParty/kcp/`, 어댑터는 `src/transport/kcp/`에 포함되어 있다.

앱은 `IServerApp`을 구현한다.

```cpp
#include "helix/helix.h"
#include <memory>

class MyApp final : public helix::IServerApp {
    void OnMessage(helix::Session& session, helix::ByteSpan message) override {
        session.Send(message);
    }
};

int main() {
    return helix::Server::LoadConfig("config/helix.toml")
        .Run(std::make_unique<MyApp>());
}
```

`OnMessage`만 필수이며 나머지 수명 훅은 선택이다. 이 최소 예제는 echo 동작만 보여 준다. 종료 시그널 처리는 `samples/echo_phase1/main.cpp`를 참고한다. 콜백의 메시지는 콜백 동안만 유효하다. 세션 원시 포인터를 장기 보관하지 않고 ID를 저장한다. ID 전송에는 `RouteToSession`을 사용한다.

## 독립 C# 서버

.NET 10 SDK가 필요하며 native DLL은 필요하지 않다.

```powershell
dotnet build csharp/Helix.sln -c Release -m:1
dotnet run --project csharp/samples/Helix.Echo -c Release -- config/helix.toml
```

`Helix.Engine`의 `HelixServer.Create` → `RunAsync(IHelixApp)` 경로를 사용한다. 클래스 또는 `DelegateServerApp`으로 콜백을 주입한다. 현재 전송은 async TCP이고 `RouteToSession`은 로컬 세션 범위다. KCP·Redis·TLS와 C++ 작업 그래프 월드는 구현되어 있지 않다. [실제 API 예제](API.md)를 참고한다.

## 설정과 통신 검증

우선순위는 코드 기본값 → 파일 → `HELIX_<SECTION>__<KEY>` 환경변수다. 예를 들어 다음 설정은 이후 같은 터미널에서 기동한 서버에 적용된다.

```powershell
$env:HELIX_NETWORK__PORT="6000"
$env:HELIX_LOGGING__LEVEL="debug"
```

사용 후 `Remove-Item Env:HELIX_NETWORK__PORT, Env:HELIX_LOGGING__LEVEL`로 덮어쓰기를 제거한다. 설정은 기동 시 읽는다. 모든 설정이 실행 중 자동 교체되는 것은 아니다.

기동한 TCP echo 서버에 다른 터미널에서 Python 3.10 이상으로 검증한다. 포트를 바꿨으면 봇에도 같은 값을 전달한다.

```powershell
python tools/echo_bot.py --port 5000 --clients 200 --messages 50 --size 256
python tools/echo_bot.py churn --port 5000 --clients 500 --rounds 20
```

결과 PASS와 종료코드 0을 함께 확인한다. C++/C# 출력이 모두 빌드된 환경의 공통 회귀 도구는 `python tools/verify_engines.py`다. 한 언어만 수정한 작업에서 이 도구 때문에 다른 엔진의 구현·빌드 범위를 확대하지 않는다.

## C++ KCP와 Redis

기동 전 `HELIX_NETWORK__TRANSPORT=kcp`로 설정하면 같은 native 앱이 KCP를 사용한다. TCP 봇 대신 `helix_kcp_echo_client`를 사용한다.

```powershell
$env:HELIX_NETWORK__TRANSPORT="kcp"
./build/windows-x64/Release/helix_echo_phase1.exe
# 별도 터미널
./build/windows-x64/Release/helix_kcp_echo_client.exe 127.0.0.1 5000 200 128
```

테스트 후 서버 터미널의 `HELIX_NETWORK__TRANSPORT` 덮어쓰기를 제거한다. Windows TCP는 `HELIX_NETWORK__BACKEND=iocp_rio`로 실제 RIO 경로를 선택한다. Linux TCP는 epoll이다. QUIC은 미구현이다.

Redis 백플레인은 C++의 선택 기능이다. Redis를 준비하고 각 노드의 `cluster.enabled=true`, 서로 다른 `cluster.node_id`·포트를 설정해 `helix_route_phase6`를 실행한다. `tools/cluster_route_bot.py --ports 5000 5001`로 전달을 확인한다. 세션 라우팅·공유 KV·데이터 버전 통지를 제공하며 한 전장의 상태를 여러 노드에 자동 분산하는 기능은 아니다. [cluster 설정과 전달 범위](CONFIGURATION.md)를 참고한다.

## 설치 SDK와 런타임 데이터

설치 빌드와 소비자 빌드의 구성·컴파일러를 맞춘다.

```powershell
cmake --install build/windows-x64 --config Release --prefix D:/sdk/helix
```

```cmake
find_package(helix CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE helix::helix)
# 오프라인 MMO: helix::mmo
# 선택 모듈을 설치한 경우: helix::mmo_parallel
```

소비자 구성에 `-DCMAKE_PREFIX_PATH=D:/sdk/helix`를 전달한다. Protocol ON SDK는 일치하는 Protobuf CMake 패키지가 필요하다. C ABI는 `HELIX_BUILD_CAPI=ON`으로 설치한 `helix::helix_capi`를 링크한다. 병렬 MMO 소비자는 Taskflow 헤더가 필요하지 않다.

C++ `Snapshot<T>`는 완성된 `shared_ptr<const T>`를 `Store`로 게시하고 `Load`로 읽는다. 진행 중 reader는 이전 게시 객체를 유지하며, 게시 후에는 내용을 변경하지 않는다. `atomic<shared_ptr>`의 lock-free 동작이나 노드 전체의 동시 게시를 보장하지 않는다. Redis 데이터 버전 통지는 각 앱이 로드·검증·게시하는 트리거다.

## 선택 콘텐츠 예제

기본 echo 앱처럼 앱이 직접 메시지를 처리할 수 있고, 별도 콘텐츠 모듈을 조립할 수도 있다.

- [MMO 콘텐츠](MMO.md): 이동·전투·가시성 규칙을 주입하는 상태 관리 예제다.
- [예제 메시지](SERIALIZATION.md): 위 콘텐츠의 공통 schema와 직렬화 adapter다. Python 클라이언트 준비는 [빌드·검증](BUILD.md)을 따른다.
- [C++ 병렬 월드](PARALLEL_MMO.md): 같은 전장의 콘텐츠 계산을 여러 worker로 실행하는 선택 실험이다. 정확성과 현재 성능 한계를 해당 문서에 기록한다.

## 성능 측정과 문제 확인

네트워크·직렬화·도메인 tick은 서로 다른 측정이다. [성능 측정 가이드](BENCHMARKS.md)와 각 도구의 `--help`를 확인하고 빌드·다른 측정을 겹쳐 실행하지 않는다. C++/C# 언어 이름만으로 GC·p99·수용 인원 우열을 단정하지 않는다.

- 설정 오류: 로그의 validation 사유와 터미널의 `HELIX_*` 덮어쓰기를 확인한다.
- `parallel world requires HELIX_BUILD_PARALLEL_MMO=ON`: 선택 모듈을 켜서 빌드하거나 `execution=sequential`로 되돌린다.
- Python Protobuf 오류: 고정 protoc 36.2와 `tools/requirements-protobuf.txt`의 runtime을 맞춘다.
- cross-node 전달 실패: Redis 연결 로그와 노드 ID 중복·포트를 확인한다.
- 빌드 후 `pwsh.exe` 메시지: 성공 여부는 MSBuild 종료코드와 실제 실행 검증으로 판단한다.
