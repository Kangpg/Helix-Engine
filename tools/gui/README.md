# Helix 테스트 대시보드 (GUI)

`tools/*.py` 검증 봇(echo / churn / **load** / cluster-route / benchmark)을 브라우저에서
실행·모니터링하는 웹 대시보드. 폼으로 인자를 채우고 ▶ 실행하면 봇을 서브프로세스로
돌리며 출력을 **실시간 스트리밍**하고, load 모드는 **라이브 차트**(처리량·지연 백분위)와
**통계 카드**로 시각화한다. 완료된 런은 **런 비교** 탭에서 표로 비교.

## 실행

```bash
pip install -r tools/gui/requirements.txt
python tools/gui/app.py            # http://127.0.0.1:8770
```

옵션: `--host 0.0.0.0`(다른 머신에서 대시보드 접속), `--port 9000`, `--debug`.

원격 서버 부하: 폼의 `host`에 서버 머신 IP를 넣으면 된다(봇은 GUI 머신에서 돈다).

## 지속 부하 (load 모드)

- `duration` 초 동안 send→recv 루프. 연결 오류 시 백오프 후 재접속하며 부하 유지.
- `rate` : 총 목표 msg/s(0=무제한). 클라이언트별로 분산 페이싱.
- `workers` : 봇 프로세스 수. clients/rate를 분배해 **파이썬 GIL 한계를 우회**
  (부하 생성기 자체가 병목이 되지 않게). 부모 프로세스가 통계를 집계한다.
- 초당 `@STAT {json}` 실시간 통계 → 대시보드가 파싱해 차트/카드 갱신.
- 종료 시 `@SUM {json}` 최종 요약 → 런 비교 테이블. (echo/churn도 @SUM은 출력)

사이드바 **빠른 부하** 버튼(라이트/미디엄/헤비/익스트림)으로 프리셋 적용 가능
(host/port는 유지됨). 강도 조합은 `tools.json`의 `quick[]`에서 수정.

## 구조

```
tools/gui/
  app.py          Flask 백엔드(서브프로세스 실행 + SSE 스트리밍 + 프리셋)
  tools.json      도구·파라미터·빠른부하 정의 ← 여기만 고치면 UI에 자동 반영
  index.html      대시보드(폼/라이브 차트/통계 카드/콘솔/런 비교)
  presets.json    저장된 프리셋(런타임 생성)
  requirements.txt
```

## 도구를 추가·수정·삭제하려면

`tools.json`의 `tools[]`만 편집하면 된다(서버 재시작 없이 브라우저 새로고침).

파라미터 스키마:

| 필드 | 의미 |
|---|---|
| `name` | 폼 필드 키 |
| `type` | `text` \| `int` \| `float` \| `bool` \| `select` |
| `default` | 기본값 |
| `flag` | 붙일 CLI 플래그(예: `--port`). 없으면 값만. |
| `positional` | `true`면 플래그 없이 위치 인자로 |
| `nargs` | `true`면 공백으로 나눈 여러 값(예: `--ports 5000 5001`) |
| `options` | `select`일 때 선택지 |
| `modes` | 지정 시 mode 셀렉트 값이 일치할 때만 폼에 표시 |
| `help` | 필드 아래 설명 |

`bool`은 store_true 규칙(참일 때만 `flag` 추가). 봇이 `@STAT `/`@SUM ` 접두어의
JSON 한 줄을 출력하면 어떤 도구든 라이브 차트/비교 테이블에 자동 연동된다.

## 동작 방식 / 성능

봇은 **각자 별도 프로세스**로 실행되므로 asyncio 부하 생성기가 자기 프로세스에서
풀 스피드로 돌고, 대시보드는 논블로킹으로 **여러 런을 동시에** 띄우고 중지할 수 있다.
`workers>1`이면 echo_bot이 자식 프로세스를 추가로 띄워 부하를 분산한다.
GUI는 오케스트레이션·스트리밍만 담당하므로 봇 성능에 오버헤드를 주지 않는다.
봇 로직(`tools/*.py`)은 CLI로도 그대로 실행 가능하다.

## 사전 준비 (도구별)

- **echo / load / benchmark**: 에코 서버(`helix_echo_phase1.exe` 등)를 먼저 띄운다(포트 5000).
- **cluster**: Redis 기동 + `helix_route_phase6` 노드 2개(포트 5000/5001, node_id 0/1).
