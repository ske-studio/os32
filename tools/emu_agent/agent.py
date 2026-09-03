#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""emu_agent — ローカル小型 LLM に NP21/W 上の OS32 を操作させる最小ドライバ

flm serve (FastFlowLM, OpenAI 互換 API, 127.0.0.1:52625) のモデルに
「JSON 1 個で行動を返す」プロトコルでエミュレータを触らせる。
4B 級のモデル向けに、ツール定義は system prompt の平文で渡し、
ネイティブ function calling には依存しない (FLM の tools 対応が
モデルごとに不安定なため)。tool_calls が返ってきた場合はそれも受ける。

実機との通信は NP21/W ai-debug 版の HTTP API。HTTP 層は
tools/np21w_mcp/np21w_client.py を流用する (curl.exe フォールバック込み)。

使い方:
  python3 agent.py run "ver を実行してバージョンを報告して" \
        [--model gemma4-it:e4b] [--max-steps 12] [--session NAME]
  python3 agent.py tail [--session NAME] [-n 20]

ログ: tools/emu_agent/logs/<session>/steps.jsonl (1 行 1 ステップ)

モデルの行動 (JSON):
  {"action":"cmd","cmd":"ls /"}        シェルコマンドを実行し出力を得る
  {"action":"key","seq":"ENTER"}       キーイベント注入 (seq または text)
  {"action":"tvram"}                   画面 80x25 をテキストで得る
  {"action":"status"}                  エミュレータの状態
  {"action":"done","report":"..."}     終了。report は人間向けの報告
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "np21w_mcp"))
import np21w_client as emu  # noqa: E402

LOGS = os.path.join(HERE, "logs")

FLM_BASE = os.environ.get("FLM_URL", "http://127.0.0.1:52625")
FLM_MODEL = os.environ.get("FLM_MODEL", "gemma4-it:e4b")

# [V3] リモート実行の curl タイムアウトは短くしない (15s 最小、長いものは 60s+)
CMD_TIMEOUT = int(os.environ.get("EMU_CMD_TIMEOUT", "90"))
LLM_TIMEOUT = int(os.environ.get("FLM_TIMEOUT", "600"))
# 小型モデルの文脈を食い潰さないよう、実機出力はこの長さで切る
OBS_LIMIT = 1600

SYSTEM_PROMPT = """You operate a retro computer (NEC PC-9801 running the OS32 shell) through an emulator.
You cannot see or touch it directly. You act ONLY by replying with exactly one JSON object and no other text.

Actions:
{"action":"cmd","cmd":"<shell command line>"}  run one shell command on OS32 and receive its output
{"action":"key","seq":"<KEY>"}                   press a key (ENTER, ESC, SPACE, UP, DOWN, F1 ...); or {"action":"key","text":"abc"} to type text
{"action":"tvram"}                               read the current text screen (80x25)
{"action":"status"}                              check whether the emulator is running
{"action":"done","report":"<what you found>"}    finish the task and report

Rules:
- One action per reply. Never explain, never add markdown, never wrap in code fences.
- Use "cmd" for anything the shell can do (ver, ls, cat, cd, echo, mkdir ...). Paths use "/" like Unix.
- Read the observation you get back before deciding the next action.
- When the task is complete, reply with "done" and quote the relevant output in the report.
- If something fails twice, stop with "done" and report the failure honestly."""


# ---------------------------------------------------------------------------
#  実機側アクション
# ---------------------------------------------------------------------------
def act_cmd(a):
    cmd = str(a.get("cmd", "")).strip()
    if not cmd:
        return "error: empty cmd"
    return emu.post("/api/cmd", cmd, timeout=CMD_TIMEOUT).decode("utf-8", "replace")


def act_key(a):
    body = []
    if a.get("seq"):
        body.append("seq=" + str(a["seq"]))
    if a.get("text"):
        body.append("text=" + str(a["text"]))
    if not body:
        return "error: key needs seq or text"
    return emu.post("/api/key", "&".join(body)).decode("utf-8", "replace")


def act_tvram(_):
    d = json.loads(emu.get("/api/tvram"))
    lines = d.get("lines", [])
    return "\n".join(l.rstrip() for l in lines)


def act_status(_):
    return emu.get("/api/status").decode("utf-8", "replace")


ACTIONS = {"cmd": act_cmd, "key": act_key, "tvram": act_tvram, "status": act_status}


# ---------------------------------------------------------------------------
#  LLM 呼び出しと応答パース
# ---------------------------------------------------------------------------
def llm_chat(model, messages):
    req = {"model": model, "messages": messages, "max_tokens": 300,
           "temperature": 0.2}
    data = json.dumps(req).encode("utf-8")
    r = urllib.request.Request(FLM_BASE + "/v1/chat/completions", data=data,
                               headers={"Content-Type": "application/json"},
                               method="POST")
    t0 = time.time()
    with urllib.request.urlopen(r, timeout=LLM_TIMEOUT) as resp:
        d = json.loads(resp.read().decode("utf-8", "replace"))
    msg = d["choices"][0]["message"]
    usage = d.get("usage", {})
    return msg, usage, time.time() - t0


_JSON_RE = re.compile(r"\{.*\}", re.S)


def parse_action(msg):
    """assistant メッセージから行動 dict を取り出す。取れなければ None"""
    # ネイティブ tool_calls が来たらそれを優先
    tc = msg.get("tool_calls") or []
    if tc:
        fn = tc[0].get("function", {})
        try:
            args = json.loads(fn.get("arguments") or "{}")
        except ValueError:
            args = {}
        name = fn.get("name", "")
        if name.startswith("emu_"):
            name = name[4:]
        args["action"] = name
        return args
    text = (msg.get("content") or "").strip()
    # ```json ... ``` を剥がす
    text = re.sub(r"^```[a-zA-Z]*\s*|\s*```$", "", text)
    m = _JSON_RE.search(text)
    if not m:
        return None
    frag = m.group(0)
    # 最初の完全な JSON オブジェクトだけ採用 (連続で 2 個出す個体がいる)
    dec = json.JSONDecoder()
    try:
        obj, _ = dec.raw_decode(frag)
    except ValueError:
        return None
    return obj if isinstance(obj, dict) else None


def clip(s):
    if len(s) <= OBS_LIMIT:
        return s
    return s[:OBS_LIMIT] + "\n...[truncated %d chars]" % (len(s) - OBS_LIMIT)


# ---------------------------------------------------------------------------
#  メインループ
# ---------------------------------------------------------------------------
def run(task, model, max_steps, session):
    os.makedirs(os.path.join(LOGS, session), exist_ok=True)
    log_path = os.path.join(LOGS, session, "steps.jsonl")
    log = open(log_path, "a", encoding="utf-8")

    def rec(**kw):
        kw["t"] = time.time()
        log.write(json.dumps(kw, ensure_ascii=False) + "\n")
        log.flush()

    messages = [{"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": "Task: " + task}]
    rec(event="start", task=task, model=model)
    print("[emu_agent] model=%s session=%s" % (model, session))
    print("[emu_agent] task: %s" % task)

    bad = 0
    for step in range(1, max_steps + 1):
        try:
            msg, usage, dt = llm_chat(model, messages)
        except (urllib.error.URLError, OSError, KeyError, ValueError) as e:
            rec(event="llm_error", step=step, error=str(e))
            print("[emu_agent] LLM error: %s" % e)
            return {"ok": False, "reason": "llm_error", "error": str(e), "steps": step}
        raw = msg.get("content") or json.dumps(msg.get("tool_calls"))
        action = parse_action(msg)
        rec(event="llm", step=step, raw=raw, action=action, usage=usage, secs=round(dt, 1))
        print("[%2d] llm %.0fs -> %s" % (step, dt, (raw or "").strip()[:200]))

        # 会話に載せるのは assistant の生テキスト (tool_calls 形式は平文化)
        messages.append({"role": "assistant", "content": raw if isinstance(raw, str) else json.dumps(raw)})

        if action is None or "action" not in action:
            bad += 1
            obs = ("Your reply was not a single JSON action object. Reply with exactly one JSON "
                   "object like {\"action\":\"cmd\",\"cmd\":\"ver\"}.")
            if bad >= 3:
                rec(event="abort", reason="unparseable")
                return {"ok": False, "reason": "unparseable", "steps": step}
            messages.append({"role": "user", "content": obs})
            continue
        bad = 0

        kind = str(action.get("action"))
        if kind == "done":
            report = str(action.get("report", ""))
            rec(event="done", step=step, report=report)
            print("[emu_agent] DONE: %s" % report)
            return {"ok": True, "report": report, "steps": step}

        fn = ACTIONS.get(kind)
        if fn is None:
            obs = "Unknown action '%s'. Valid: cmd, key, tvram, status, done." % kind
        else:
            try:
                obs = fn(action)
            except Exception as e:  # noqa: BLE001 — 実機側の失敗はそのまま観測として返す
                obs = "error: %s" % e
        obs = clip(obs)
        rec(event="obs", step=step, action=action, obs=obs)
        print("     obs: %s" % obs.strip().replace("\n", " | ")[:200])
        messages.append({"role": "user", "content": "Observation:\n" + obs})

    rec(event="abort", reason="max_steps")
    return {"ok": False, "reason": "max_steps", "steps": max_steps}


def tail(session, n):
    p = os.path.join(LOGS, session, "steps.jsonl")
    if not os.path.exists(p):
        print("no log: %s" % p)
        return 1
    with open(p, encoding="utf-8") as f:
        lines = f.readlines()
    sys.stdout.write("".join(lines[-n:]))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("task")
    r.add_argument("--model", default=FLM_MODEL)
    r.add_argument("--max-steps", type=int, default=12)
    r.add_argument("--session", default=time.strftime("%Y%m%d-%H%M%S"))
    t = sub.add_parser("tail")
    t.add_argument("--session", default=None)
    t.add_argument("-n", type=int, default=20)
    args = ap.parse_args()

    if args.cmd == "run":
        res = run(args.task, args.model, args.max_steps, args.session)
        res["session"] = args.session
        print("RESULT: " + json.dumps(res, ensure_ascii=False))
        return 0 if res.get("ok") else 1
    if args.cmd == "tail":
        s = args.session
        if s is None:
            sessions = sorted(os.listdir(LOGS)) if os.path.isdir(LOGS) else []
            if not sessions:
                print("no sessions")
                return 1
            s = sessions[-1]
        return tail(s, args.n)
    return 2


if __name__ == "__main__":
    sys.exit(main())
