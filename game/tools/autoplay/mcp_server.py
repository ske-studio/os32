#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""autoplay MCP サーバ (stdio JSON-RPC 2.0)

ローカルAI (flm/gemma) によるゲーム自動プレイを Claude から制御・観測する。
実体は同ディレクトリの driver.py / flm_serve.py で、このサーバは薄い
ラッパー。CLI でも同じことができる (driver.py start/status/tail/stop)。

.mcp.json 登録例:
  "autoplay": {"command": "python3", "args": ["tools/autoplay/mcp_server.py"]}
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.path.join(HERE, "driver.py")
FLM = os.path.join(HERE, "flm_serve.py")

TOOLS = [
    {
        "name": "autoplay_start",
        "description": "ゲーム自動プレイを開始する (バックグラウンド)。"
                       "flm serve が必要 (flm_status で確認)。",
        "inputSchema": {
            "type": "object",
            "properties": {
                "policy": {"type": "string", "enum": ["llm", "scripted"],
                           "description": "llm=gemmaが選ぶ / scripted=決定的"},
                "max_decisions": {"type": "integer"},
                "goal": {"type": "string",
                         "description": "gemma に与える目的文"},
            },
        },
    },
    {
        "name": "autoplay_status",
        "description": "自動プレイの稼働状態と最新の決定を返す",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "autoplay_tail",
        "description": "決定ログ (JSONL) の末尾 n 件を返す",
        "inputSchema": {
            "type": "object",
            "properties": {"n": {"type": "integer"}},
        },
    },
    {
        "name": "autoplay_stop",
        "description": "自動プレイを停止する",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "flm_status",
        "description": "flm サーバ (ローカルLLM) の稼働状態",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "flm_start",
        "description": "flm serve を起動する (モデルロードに数十秒)",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def run_cli(args, timeout=300):
    p = subprocess.run([sys.executable] + args, capture_output=True,
                       timeout=timeout, text=True)
    out = (p.stdout or "") + (p.stderr or "")
    return out.strip() or "(no output)"


def call_tool(name, args):
    if name == "autoplay_start":
        cmd = [DRIVER, "start"]
        if args.get("policy"):
            cmd += ["--policy", args["policy"]]
        if args.get("max_decisions"):
            cmd += ["--max-decisions", str(args["max_decisions"])]
        if args.get("goal"):
            cmd += ["--goal", args["goal"]]
        # バックグラウンド起動 (このプロセスは待たない)
        logdir = os.path.join(HERE, "logs")
        os.makedirs(logdir, exist_ok=True)
        out = open(os.path.join(logdir, "driver.out"), "a")
        subprocess.Popen([sys.executable] + cmd, stdout=out, stderr=out,
                         start_new_session=True)
        return "started in background. use autoplay_status / autoplay_tail."
    if name == "autoplay_status":
        return run_cli([DRIVER, "status"], timeout=30)
    if name == "autoplay_tail":
        return run_cli([DRIVER, "tail", "-n", str(args.get("n", 10))],
                       timeout=30)
    if name == "autoplay_stop":
        return run_cli([DRIVER, "stop"], timeout=30)
    if name == "flm_status":
        return run_cli([FLM, "status"], timeout=30)
    if name == "flm_start":
        return run_cli([FLM, "start"], timeout=300)
    raise ValueError("unknown tool: %s" % name)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        rid = req.get("id")
        method = req.get("method")
        resp = None
        if method == "initialize":
            resp = {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "autoplay", "version": "1.0.0"},
            }
        elif method == "tools/list":
            resp = {"tools": TOOLS}
        elif method == "tools/call":
            name = req["params"]["name"]
            args = req["params"].get("arguments") or {}
            try:
                text = call_tool(name, args)
                resp = {"content": [{"type": "text", "text": text}]}
            except Exception as e:
                resp = {"content": [{"type": "text",
                                     "text": "error: %s" % e}],
                        "isError": True}
        elif method in ("notifications/initialized", "ping"):
            if rid is None:
                continue
            resp = {}
        else:
            if rid is None:
                continue
            sys.stdout.write(json.dumps({
                "jsonrpc": "2.0", "id": rid,
                "error": {"code": -32601, "message": "unknown method"},
            }) + "\n")
            sys.stdout.flush()
            continue
        if rid is not None:
            sys.stdout.write(json.dumps({
                "jsonrpc": "2.0", "id": rid, "result": resp,
            }, ensure_ascii=False) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
