# Helix Engine

**C++과 C#으로 각각 사용할 수 있는 범용 네트워크 서버 엔진입니다.**

연결 관리, 메시지 송수신, 프레이밍, 송신 큐와 설정을 처리합니다. 앱은 메시지를 받았을 때 실행할 로직을 작성합니다. 채팅 서버, 게임 서버, 내부 TCP 서비스처럼 연결을 유지하며 메시지를 주고받는 앱에 사용할 수 있습니다.

## 주요 기능

- **메시지 단위 처리** — TCP 수신 데이터를 길이 헤더로 나눠 핸들러에 전달합니다.
- **세션 관리** — 접속·종료와 세션별 송신 큐·백프레셔를 처리합니다.
- **외부 설정** — 파일과 환경변수로 포트, 전송 방식, 버퍼와 실행 한도를 지정합니다.
- **독립 구현** — C++과 C# 중 필요한 엔진을 선택해 사용합니다.

| 구분 | C++ | C# |
|---|---|---|
| 환경 | C++20 · Windows/Linux | .NET 10 |
| 기본 전송 | TCP · Windows IOCP/RIO, Linux epoll | async TCP |
| 추가 전송 | KCP | TCP |
| 노드 간 세션 라우팅 | 선택 Redis 백플레인 | 로컬 세션 범위 |

C#은 네이티브 DLL 없이 실행합니다. C ABI는 C++의 선택 빌드입니다. QUIC과 C#의 KCP·Redis·TLS는 미구현입니다.

## 빠른 시작

```powershell
git clone https://github.com/Kangpg/helix-engine.git
cd helix-engine
```

### C++

Windows는 Visual Studio 2022 C++ 도구와 CMake 3.21 이상이 필요합니다.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-release --parallel 8
./build/windows-x64/Release/helix_echo_phase1.exe
```

기본 빌드는 예제용 프로토콜 의존성을 내려받습니다. 프로토콜을 제외하는 구성과 Linux 빌드는 [빌드 가이드](docs/BUILD.md)를 참고하세요.

### C#

.NET 10 SDK가 필요합니다. C++ 빌드와 별도로 실행합니다.

```powershell
dotnet run --project csharp/samples/Helix.Echo -c Release -- config/helix.toml
```

## 앱 작성 방식

C++은 `IServerApp`의 메시지 핸들러를 구현합니다. 다음은 받은 내용을 그대로 돌려주는 최소 예제입니다.

```cpp
#include "helix/helix.h"
#include <memory>

class Echo final : public helix::IServerApp {
    void OnMessage(helix::Session& session, helix::ByteSpan message) override {
        session.Send(message);
    }
};

int main() {
    return helix::Server::LoadConfig("config/helix.toml")
        .Run(std::make_unique<Echo>());
}
```

C#은 `IHelixApp` 또는 델리게이트로 같은 역할의 핸들러를 작성합니다.

```csharp
using Helix.Engine;

await using var server = HelixServer.Create("config/helix.toml");
await server.RunAsync(new DelegateServerApp((session, message) =>
{
    session.Send(message);
}));
```

접속·종료 콜백, 종료 처리, 오류 확인과 외부 프로젝트 연결은 [사용 가이드](docs/USAGE.md)와 [API](docs/API.md)에 있습니다.

## 설정

기본 파일은 [config/helix.toml](config/helix.toml)입니다. `HELIX_NETWORK__PORT=6000`처럼 `HELIX_<섹션>__<키>` 환경변수로 값을 덮어씁니다. [설정 항목](docs/CONFIGURATION.md)을 참고하세요.

## 확장 모듈과 예제

앱의 메시지 형식과 콘텐츠 처리는 엔진 위에 조립합니다.

| 모듈·예제 | 용도 |
|---|---|
| Echo · Chat | 기본 송수신과 채팅 앱 예제 · `samples/`, `csharp/samples/` |
| [Protocol](docs/SERIALIZATION.md) | 예제 메시지와 Protobuf·FlatBuffers·C# MemoryPack adapter |
| [MMO 콘텐츠](docs/MMO.md) | 이동·전투·가시성 규칙을 주입하는 선택 모듈 |
| [C++ 병렬 월드](docs/PARALLEL_MMO.md) | 콘텐츠 계산을 작업 그래프로 나누는 선택 실험 |

MMO 모듈은 엔진의 활용 예제입니다. 병렬 실험의 정확성·측정 결과와 제약은 해당 문서에 기록합니다.

## 문서

[빌드·검증](docs/BUILD.md) · [사용 가이드](docs/USAGE.md) · [API](docs/API.md) · [성능 측정](docs/BENCHMARKS.md) · [전체 문서](docs/README.md)

구현 구조는 [코드 지도](docs/CODE_MAP.md), 개발 이력은 [계획과 기록](HELIX_ENGINE_PLAN.md)에 있습니다.

## 라이선스

[MIT](LICENSE). 포함하거나 복원하는 외부 라이브러리의 고지는 LICENSE와 설치 SDK의 `share/helix/licenses`에 제공합니다.
