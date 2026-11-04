# 성능 측정

> 갱신: 2026-11-04. 모든 명령은 저장소 루트 기준이다.

측정 목적에 맞는 도구를 선택한다. 연결·송수신 성능, 직렬화 비용, 콘텐츠 계산 시간은 각각 따로 측정한다. 빌드와 다른 측정이 끝난 같은 환경에서 순차 실행한다.

| 대상 | 도구·절차 | 측정 범위 |
|---|---|---|
| TCP echo·연결 반복 | `tools/echo_bot.py` | 실제 메시지 왕복·연결 수명 |
| Windows IOCP/RIO 비교 | `tools/backend_benchmark.py` | 같은 부하의 native TCP 경로 |
| 직렬화 포맷 | 아래 bare-state 비교 | 별도 DTO의 encode/decode·크기·할당 |
| 실제 codec | 아래 헤더·검증 포함 비교 | 운영 adapter와 framing·검증 비용 |
| 선택 병렬 콘텐츠 | [병렬 월드 측정](PARALLEL_MMO.md) | 같은 단계 규칙의 inline/worker tick·별도 AOI 질의 |

## 네트워크

Release echo 서버를 실행한 뒤 다른 터미널에서 같은 부하로 비교한다.

```powershell
./build/windows-x64/Release/helix_echo_phase1.exe
# 다른 터미널
python tools/echo_bot.py --port 5000 --clients 500 --messages 200 --size 256
python tools/echo_bot.py churn --port 5000 --clients 500 --rounds 20
```

설정·백엔드·메시지 크기·클라이언트 수를 결과와 함께 남긴다. echo의 왕복 수치는 특정 업무나 게임의 수용 인원을 뜻하지 않는다. 실행별 보고서와 원시는 로컬 `out/`에 생성하며 Git에는 포함하지 않는다.

## 직렬화 성능 비교

기본 운영 wire는 Protobuf를 유지한다. 선택 직렬화 adapter는 [직렬화 계약](SERIALIZATION.md)에 있고 아래 benchmark는 별도 DTO의 측정이다. 새 adapter의 envelope·verifier·VersionTolerant 비용을 포함한 측정은 아니다. C#은 Protobuf·MessagePack·MemoryPack·JSON·FlatBuffers·Binary, C++은 Protobuf·FlatBuffers·Binary를 비교한다. 같은 월드 상태/AOI 배열 0·16·256·4096개로 encode/decode를 분리하고 크기·시간, C#은 관리 할당량도 기록한다. 전체 객체/배열 복원을 비교하며 FlatBuffers view-only 조회와 혼합하지 않는다.

```powershell
dotnet run --project csharp/benchmarks/Helix.SerializationBenchmarks -c Release -- --quick
# BenchmarkDotNet 정식 측정 (시간이 더 필요)
dotnet run --project csharp/benchmarks/Helix.SerializationBenchmarks -c Release -- --filter "*" --artifacts out/serialization-bdn
# 공식 flatc 25.2.10 경로를 전달하는 선택 C++ benchmark
cmake --preset windows-x64 -DHELIX_BUILD_BENCHMARKS=ON -DHELIX_FLATC_EXECUTABLE=D:/tools/flatc.exe
cmake --build --preset windows-release --target helix_serialization_benchmark helix_echo_phase1
./build/windows-x64/Release/helix_serialization_benchmark.exe 10000 9 out/serialization-native.csv
python tools/backend_benchmark.py
python tools/serialization_report.py
```

FlatBuffers C# runtime/flatc는 25.2.10으로 맞춘다. 비교용 `.fbs`와 generated C# 파일은 benchmark에 있고 재생성은 `tools/generate_benchmark_schema.ps1 -Flatc <path>`로 한다. C++ 생성 파일은 build 안에만 둔다. 예비 JSON/CSV와 통합 보고서는 `out/`에 저장한다. 모든 비교를 동시에 실행하지 말고 빌드가 끝난 같은 환경에서 순차 실행한다. quick 결과와 loopback 수치를 대규모 MMO 성능으로 일반화하지 않는다.

.NET 10 전환 후의 측정과 선택 결론은 로컬 보고서 `out/serialization-report.md`에 생성된다. 기존 .NET 8 자료는 `out/net8-baseline/`에 보존한다. runtime 비교를 넣으려면 `python tools/serialization_report.py --baseline out/serialization-net8-recheck.json`을 실행한다. 이 bare-state 측정에서 C# MemoryPack과 큰 상태의 공통 FlatBuffers를 후속 후보로 선정했다. 실제 adapter의 현재 판단은 아래 헤더·검증 포함 비교를 따른다. 직접 Binary는 fixed-layout 비교 기준이고 전체 프로토콜을 자동 교체하지 않는다.

## 적용 codec의 헤더·검증 포함 비교

실제 registry/adapter에 4B TCP 길이 prefix 생성·검사, 선택 8B envelope, FlatBuffers verifier/MemoryPack layout 검사 및 공통 의미 검증을 포함한 결과는 로컬 보고서 `out/wire-report.md`에 생성된다. 새 소유 frame/전체 DTO 복원 기준이며 socket·큐·Stream 수신 allocation·tick은 제외한다. 이전 bare-state 자료는 그대로 보존한다.

C#은 별도 `Helix.WireBenchmarks`에서 BenchmarkDotNet 54개 조건과 managed allocation, C++은 같은 Release DTO/packet의 Protobuf/FlatBuffers 시간과 별도 operator-new 요청 bytes/calls probe를 측정한다. native alloc은 direct malloc/OS 및 peak 메모리가 아니며 managed alloc과 같은 지표가 아니다.

현재 adapter 측정에서는 공통 기본 Protobuf를 유지한다. C#의 큰 상태에서는 MemoryPack이 속도 후보지만 크기·encode 할당이 늘고, FlatBuffers는 C++의 큰 상태에서 이점이 있어도 현재 C# 전체 복원이 느려 공통 큰 snapshot의 기본 권장은 보류한다. 언어별 상세 수치·조건은 [Phase 10의 헤더·검증 포함 측정 기록](phases/PHASE10.md)에 보존한다.

```powershell
dotnet build csharp/benchmarks/Helix.WireBenchmarks/Helix.WireBenchmarks.csproj -c Release -m:1
dotnet csharp/benchmarks/Helix.WireBenchmarks/bin/Release/net10.0/Helix.WireBenchmarks.dll --smoke
cmake --preset windows-x64 -DHELIX_BUILD_BENCHMARKS=ON
cmake --build --preset windows-release --target helix_wire_benchmark helix_wire_allocation_probe
python tools/wire_native_benchmark.py
# 위 native 측정/빌드가 끝난 뒤 순차 실행
dotnet csharp/benchmarks/Helix.WireBenchmarks/bin/Release/net10.0/Helix.WireBenchmarks.dll --out out/wire-managed.json
python tools/wire_benchmark_report.py
```

클라이언트는 Unity 또는 Unreal이 될 수 있다는 공통 기준으로 선택한다. 서버 언어에 관계없이 공통 기본은 Protobuf, 공통 선택 최적화는 FlatBuffers, MemoryPack은 C# 전용 경로다. 현재 .NET 10/MSVC 결과는 해당 codec 구현의 수치이며 Unity IL2CPP·Unreal 실행 수치가 아니다. 현재 net10.0 NuGet은 서버용이고 게임 엔진용 client SDK는 별도 구성·검증 대상이다. [클라이언트 호환 전제](SERIALIZATION.md)를 따른다. 크기·할당 tradeoff와 기존 원시는 보존하고 언어 간 CPU 합을 공통 클라이언트 성능으로 쓰지 않는다.
