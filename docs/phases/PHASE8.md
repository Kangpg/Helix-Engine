# Phase 8 — C# SDK (C ABI 바인딩)

> 기록일: 2026-06-01 · 상태: 코드 완성(C++ DLL 빌드 검증 권장, C# 런타임 검증 대기)

## 목표
방금 완성한 C ABI 위에 C# 바인딩을 올려, C# 업체가 **C++와 동일한 핸들러 모델**(OnConnect/OnMessage/OnDisconnect)로 서버를 작성하게 한다. "C# 서버도 같은 방식으로 지원" 목표 달성. (계획서 Phase 8)

## 완료한 작업
- `CMakeLists.txt` — **수정**: `helix_capi` **SHARED** 타깃 추가. 엔진 전체 + `helix_c.cpp`를 `HELIX_BUILD_DLL`로 빌드 → `helix.dll`이 `helix_*` 심볼 export. 출력명 `helix`.
- `csharp/Helix/Native.cs` — **신규**: `helix_c.h` 1:1 P/Invoke. `HelixStatus`/`HelixBytes`/`HelixCallbacks` + 콜백 델리게이트(`UnmanagedFunctionPointer`, Cdecl).
- `csharp/Helix/HelixServer.cs` — **신규**: 매니지드 래퍼.
  - `IHelixApp`(OnConnect/OnMessage/OnDisconnect), `Session`(Send/Close/Id), `CloseReason`.
  - `HelixServer.Create(path)` → `Run(app)`(블로킹) → `Stop()`. 콜백 thunk가 `ctx`(GCHandle)로 인스턴스 복원 → `IHelixApp` 호출.
  - 델리게이트를 필드로 보관(GC 수집 방지), `Dispose`에서 핸들/GCHandle 해제.
  - `Send`는 `ReadOnlySpan<byte>`를 `fixed` 핀 후 `helix_session_send`.
- `csharp/Helix/Program.cs` — **신규**: 에코 샘플(`EchoApp : IHelixApp`, Ctrl-C→graceful Stop). C++ 샘플과 동일 구조.
- `csharp/Helix/Helix.csproj` — **신규**: net8.0, x64, AllowUnsafeBlocks.

## 설계 결정
- **단일 C ABI, 두 래퍼** — C++/C#가 같은 `helix.dll`/C ABI를 감싼다(유지보수 1곳). msquic·GNS 방식.
- **콜백 수명/스레딩** — 델리게이트 루팅 + `ctx`=GCHandle 패턴. 콜백은 네이티브 워커 스레드에서 호출(문서화). 동기 콜백 모델(C++와 동일).
- **Span 기반 무복사 경로** — OnMessage는 네이티브 버퍼를 `ReadOnlySpan<byte>`로 바로 노출(콜백 범위 한정). 보관 시 복사.

## 미완료 / 다음 단계로 이월
- NuGet 패키징(런타임 `helix.dll` 동봉), `helix_session_remote` 래핑(원격주소), async/Task API(액터 프레임워크 연동용) — 후속.
- 콜백 thunk에서 예외가 네이티브로 전파되지 않도록 try/catch 가드 — 보강 권장.
- `Session` 객체 매 콜백 생성 → 풀링/캐싱 최적화(후속).

## 검증 상태
**C# 런타임 검증 대기.** 절차:
1. C++ DLL 빌드: `cmake --build --preset windows-debug` → `helix.dll` 생성 확인(`build/windows-x64/Debug/helix.dll`).
2. C# 빌드/실행: `helix.dll`과 `config/`를 C# 실행 폴더로 복사 후
   ```powershell
   cd csharp/Helix
   dotnet run -c Debug
   ```
   `Helix C# echo (Phase 8) — ABI v1` + `listening on ...`(네이티브 로그) 확인.
3. 봇: `python tools/echo_bot.py --port 5000 --clients 200 --messages 50` → PASS면 C# 경로로 프레임 에코 정상.
4. Ctrl-C → graceful drain 종료.

주의: `DllNotFoundException`이면 `helix.dll`이 작업 디렉터리/출력 폴더에 없음 → 복사하거나 PATH 설정.

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md
- C ABI: ../../include/helix/helix_c.h · 이전 단계: ./PHASE5.md
