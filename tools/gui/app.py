#!/usr/bin/env python3
# =============================================================================
# Helix 테스트 대시보드 — Flask 백엔드
#
# 기존 CLI 봇(tools/*.py)을 웹 GUI로 대체하는 실행 오케스트레이터.
#   - 봇은 서브프로세스로 실행 → 각 봇의 asyncio 루프가 자기 프로세스에서
#     풀 성능으로 돌고(진짜 병렬), GUI는 논블로킹으로 다중 실행/중지 가능.
#   - 출력은 SSE(Server-Sent Events)로 브라우저에 실시간 스트리밍.
#   - 도구/파라미터 정의는 tools.json(외부화) → 자유롭게 추가·수정·삭제.
#   - 파라미터 조합은 프리셋으로 저장(presets.json).
#
# 실행:
#   pip install -r tools/gui/requirements.txt
#   python tools/gui/app.py            # http://127.0.0.1:8770
#   python tools/gui/app.py --port 9000 --host 0.0.0.0
#
# 의존성: flask (봇 자체는 여전히 표준 라이브러리만 사용).
# =============================================================================

import argparse
import json
import os
import shlex
import subprocess
import sys
import threading
import time
import uuid
from collections import deque

from flask import Flask, Response, jsonify, request, send_from_directory

# --- 경로 -------------------------------------------------------------------
GUI_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(GUI_DIR))   # tools/gui -> tools -> repo
TOOLS_JSON = os.path.join(GUI_DIR, "tools.json")
PRESETS_JSON = os.path.join(GUI_DIR, "presets.json")

app = Flask(__name__, static_folder=None)

# --- 실행 레지스트리 ---------------------------------------------------------
# run_id -> {proc, tool_id, cmd, status, code, lines(deque), started, cond}
RUNS = {}
RUNS_LOCK = threading.Lock()
MAX_LINES = 5000          # 런당 버퍼 상한(메모리 보호)
MAX_HISTORY = 40          # 보관할 최근 런 수


def _load_tools():
    with open(TOOLS_JSON, "r", encoding="utf-8") as f:
        return json.load(f)


def _load_presets():
    if not os.path.exists(PRESETS_JSON):
        return []
    try:
        with open(PRESETS_JSON, "r", encoding="utf-8") as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return []


def _save_presets(presets):
    with open(PRESETS_JSON, "w", encoding="utf-8") as f:
        json.dump(presets, f, ensure_ascii=False, indent=2)


def _find_tool(tool_id):
    for t in _load_tools().get("tools", []):
        if t["id"] == tool_id:
            return t
    return None


def _build_cmd(tool, values):
    """tools.json 정의 + 사용자 값 -> 커맨드라인 인자 리스트."""
    argv = [sys.executable, os.path.join(REPO_ROOT, tool["script"])]
    positionals, flags = [], []
    for p in tool.get("params", []):
        name = p["name"]
        val = values.get(name, p.get("default"))
        ptype = p.get("type", "text")

        if ptype == "bool":
            if str(val).lower() in ("true", "1", "on", "yes"):
                flags.append(p["flag"])
            continue

        # 빈 값은 스킵(기본값 사용). 단 positional은 반드시 넣는다.
        if (val is None or str(val) == "") and not p.get("positional"):
            continue

        if p.get("positional"):
            positionals.append(str(val))
        elif p.get("nargs"):
            # 공백으로 분리된 여러 값
            flags.append(p["flag"])
            flags.extend(shlex.split(str(val)))
        else:
            flags.append(p["flag"])
            flags.append(str(val))

    return argv + positionals + flags


def _reader(run_id, proc):
    """서브프로세스 stdout을 줄 단위로 읽어 버퍼+대기자에 통지."""
    rec = RUNS[run_id]
    try:
        for raw in iter(proc.stdout.readline, b""):
            line = raw.decode("utf-8", errors="replace").rstrip("\n")
            with rec["cond"]:
                rec["lines"].append(line)
                rec["cond"].notify_all()
    finally:
        proc.stdout.close()
        code = proc.wait()
        with rec["cond"]:
            rec["status"] = "done"
            rec["code"] = code
            rec["cond"].notify_all()


def _gc_runs():
    """오래된 완료 런 정리."""
    with RUNS_LOCK:
        done = [(rid, r["started"]) for rid, r in RUNS.items() if r["status"] == "done"]
        done.sort(key=lambda x: x[1])
        while len(done) > MAX_HISTORY:
            rid, _ = done.pop(0)
            RUNS.pop(rid, None)


# --- 정적/페이지 ------------------------------------------------------------
@app.route("/")
def index():
    return send_from_directory(GUI_DIR, "index.html")


# --- API --------------------------------------------------------------------
@app.route("/api/tools")
def api_tools():
    return jsonify(_load_tools())


@app.route("/api/presets", methods=["GET", "POST", "DELETE"])
def api_presets():
    if request.method == "GET":
        return jsonify(_load_presets())

    presets = _load_presets()
    if request.method == "POST":
        body = request.get_json(force=True)
        name = (body.get("name") or "").strip()
        if not name:
            return jsonify({"error": "name required"}), 400
        # 동명 프리셋은 덮어쓰기
        presets = [p for p in presets if p.get("name") != name]
        presets.append({"name": name, "tool": body["tool"], "values": body["values"]})
        _save_presets(presets)
        return jsonify(presets)

    # DELETE
    name = request.args.get("name", "")
    presets = [p for p in presets if p.get("name") != name]
    _save_presets(presets)
    return jsonify(presets)


@app.route("/api/run", methods=["POST"])
def api_run():
    body = request.get_json(force=True)
    tool = _find_tool(body.get("tool", ""))
    if tool is None:
        return jsonify({"error": "unknown tool"}), 400

    cmd = _build_cmd(tool, body.get("values", {}))
    # 자식 프로세스 stdout 을 UTF-8 로 강제(Windows 기본 cp949 에서 em-dash 등 인코딩 오류 방지).
    # 리더는 UTF-8 로 디코드하므로 일치시킨다.
    child_env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    try:
        proc = subprocess.Popen(
            cmd,
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
            env=child_env,
        )
    except OSError as e:
        return jsonify({"error": f"spawn failed: {e}"}), 500

    run_id = uuid.uuid4().hex[:12]
    rec = {
        "proc": proc,
        "tool_id": tool["id"],
        "tool_name": tool["name"],
        "cmd": cmd,
        "status": "running",
        "code": None,
        "lines": deque(maxlen=MAX_LINES),
        "started": time.time(),
        "cond": threading.Condition(),
    }
    with RUNS_LOCK:
        RUNS[run_id] = rec
    threading.Thread(target=_reader, args=(run_id, proc), daemon=True).start()
    _gc_runs()

    # 표시용 커맨드 문자열(python 경로는 짧게)
    disp = " ".join(["python", tool["script"]] + cmd[2:])
    return jsonify({"run_id": run_id, "cmd": disp})


@app.route("/api/stream/<run_id>")
def api_stream(run_id):
    rec = RUNS.get(run_id)
    if rec is None:
        return jsonify({"error": "unknown run"}), 404

    def gen():
        idx = 0
        yield "retry: 2000\n\n"
        while True:
            with rec["cond"]:
                while idx >= len(rec["lines"]) and rec["status"] == "running":
                    rec["cond"].wait(timeout=1.0)
                lines = list(rec["lines"])[idx:]
                idx = len(rec["lines"])
                status = rec["status"]
                code = rec["code"]
            for ln in lines:
                yield f"data: {json.dumps({'line': ln})}\n\n"
            if status == "done":
                yield f"event: end\ndata: {json.dumps({'code': code})}\n\n"
                return
            if not lines:
                yield ": keepalive\n\n"   # 프록시 타임아웃 방지

    return Response(gen(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.route("/api/runs")
def api_runs():
    with RUNS_LOCK:
        out = [{
            "run_id": rid,
            "tool_id": r["tool_id"],
            "tool_name": r["tool_name"],
            "status": r["status"],
            "code": r["code"],
            "started": r["started"],
        } for rid, r in sorted(RUNS.items(), key=lambda x: x[1]["started"], reverse=True)]
    return jsonify(out)


@app.route("/api/stop/<run_id>", methods=["POST"])
def api_stop(run_id):
    rec = RUNS.get(run_id)
    if rec is None:
        return jsonify({"error": "unknown run"}), 404
    if rec["status"] == "running":
        rec["proc"].terminate()
    return jsonify({"ok": True})


def main():
    ap = argparse.ArgumentParser(description="Helix 테스트 대시보드")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()
    print(f"[Helix GUI] http://{args.host}:{args.port}  (repo: {REPO_ROOT})")
    app.run(host=args.host, port=args.port, debug=args.debug, threaded=True)


if __name__ == "__main__":
    main()
