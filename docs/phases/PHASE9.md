# Phase 9 — 상품화 (install/export · 문서 · 벤치마크)

> 기록일: 2026-06-01 · 상태: 1차 완료(문서·패키징). 라이선스 확정/추가 샘플은 후속.
> 외부 의존성 없는 작업이라 빌드 리스크 낮음.

## 목표
엔진을 "구매자가 바로 쓸 수 있는" 형태로 마감한다: `find_package(helix)` 소비 가능한
설치/export, SemVer 버전, 사용 문서(퀵스타트/설정/API), 벤치마크 절차.

## 완료한 작업

### 패키징 (install / export)
- `CMakeLists.txt` — **수정**: `install(TARGETS helix helix_capi EXPORT helixTargets ...)`, 공개 헤더(`include/`) 설치, `install(EXPORT ... NAMESPACE helix::)`, `helixConfig.cmake` + `helixConfigVersion.cmake`(SemVer, `SameMajorVersion`), 기본 `config/helix.toml` 설치. 타깃에 `$<INSTALL_INTERFACE:include>` 추가.
- `cmake/helixConfig.cmake.in` — **신규**: 패키지 설정 템플릿.
- 소비자 측:
  ```cmake
  find_package(helix CONFIG REQUIRED)
  target_link_libraries(myapp PRIVATE helix::helix)      # 정적 코어
  ```
- 설치: `cmake --install build/windows-x64 --config Release --prefix <dir>`

### 문서
- `README.md` — **재작성**: 낡은 HelixCore/GameServer/INI 설명 제거 → 신규 엔진(개요·특징·전송 선택표·30초 퀵스타트·레이아웃·빌드·문서 링크).
- `docs/CONFIGURATION.md` — **신규**: 전 섹션/키 레퍼런스(`[network]`/`[buffers]`/`[framing]`/`[logging]`/`[shutdown]`/`[cluster]`/`[quic]`) + env 오버라이드 규칙 + 전송/백엔드/scale-out 가이드 + 전체 예시.
- `docs/API.md` — **신규**: 공개 C++ SDK(`Server`/`IServerApp`/`Session`/`Snapshot`/types) + C ABI(`helix_c.h`) 레퍼런스, 최소 예제.

### 벤치마크 절차
기존 검증 봇(`tools/echo_bot.py`)이 처리량·지연을 보고한다(별도 도구 불필요).

```powershell
# 처리량/지연 (echo 모드: msg/s, MB/s, p50/p95/p99)
python tools\echo_bot.py --port 5000 --clients 500 --messages 200 --size 256

# 연결 수명 스트레스 (churn)
python tools\echo_bot.py churn --port 5000 --clients 1000 --rounds 50
```

- 전송별 비교: 서버를 `transport=tcp` / `kcp`로 각각 띄우고 동일 봇 부하로 측정.
  (KCP는 UDP라 봇이 직접 못 침 → KCP 처리량은 `helix_kcp_echo_client`의 왕복/시간으로 측정하거나 봇은 TCP 기준.)
- scale-out: 다중 노드 + L4 LB 뒤에서 노드 추가/제거 무중단 + 크로스노드 라우팅 지연.

## 후속 작업 (2026-06-01 추가 완료)
- **라이선스 확정** — **MIT**(사용자 결정). `LICENSE` 작성(저작권 2026 필균) + third-party 고지(임베드 KCP=MIT, skywind3000). README/NuGet 메타데이터에 MIT 반영.
- **도메인 샘플 추가** — `samples/chat_room/main.cpp`(다세션 브로드캐스트, `Server::FindSession`로 전 멤버 송신). CMake `helix_chat_room` 타깃. 전송 무관(TCP/KCP).
- **벤치마크 자동화** — `tools/benchmark.py`: echo_bot을 설정 매트릭스로 스윕 → CSV+markdown 리포트, 라벨별(tcp/kcp) 비교. 전제: 서버 기동 상태.
- **C# NuGet** — `csharp/Helix.Sdk/Helix.Sdk.csproj`: 기존 에코 exe 안 건드리고 SDK 소스(HelixServer.cs/Native.cs)만 classlib로 묶어 패킹, 네이티브 `helix.dll`을 `runtimes/win-x64/native`에 동봉. `dotnet pack -c Release`.

## 남은 후속
- Linux(Phase 4) 완료 후 크로스플랫폼 install/도커 이미지 매트릭스.
- 벤치마크 리포트를 CI에 연동(현재는 수동 실행).
- NuGet 게시(피드/버전 정책) + 샘플 추가(룸 매치 등).

## 검증 상태
문서/패키징은 **빌드 영향 없음**(install 규칙은 `cmake --install` 시에만 동작).
- `cmake --preset windows-x64` 재구성 후 `cmake --build` 정상 → `cmake --install ... --prefix out` 으로 `out/include/helix`, `out/lib/helix.lib`, `out/lib/cmake/helix/helixConfig.cmake`, `out/bin/helix.dll` 생성 확인.

## 관련 문서
- 계획: ../../HELIX_ENGINE_PLAN.md (9 상품화)
- README/CONFIGURATION/API: ../../README.md · ../CONFIGURATION.md · ../API.md
