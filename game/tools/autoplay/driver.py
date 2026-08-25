#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""対戦スゴロクRPG 自動プレイドライバ (バグ探し優先)

ゲームが物理 0x90000 に書く状態メールボックス (programs/apps/game/
view_export.c と一対) をエミュレータの /api/mem で読み、ローカルAI
(flm serve の gemma4-it:e4b, OpenAI互換 API) に次のキーを選ばせて
/api/key で注入する。全決定と異常を JSONL に記録する。

使い方:
  python3 driver.py start [--policy llm|scripted] [--max-decisions N]
                          [--goal TEXT] [--session NAME]
  python3 driver.py status
  python3 driver.py tail [-n N]
  python3 driver.py stop

観測:
  tools/autoplay/logs/<session>/decisions.jsonl   全決定 (1行1決定)
  tools/autoplay/logs/<session>/anomalies.jsonl   異常のみ
  tools/autoplay/logs/<session>/shots/            定期スクリーンショット
"""

import argparse
import json
import os
import re
import signal
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "np21w_mcp"))
import np21w_client as emu  # noqa: E402  (curl.exe 経由の HTTP 層を再利用)

LOGS = os.path.join(HERE, "logs")
PIDFILE = os.path.join(LOGS, "driver.pid")

MAILBOX_ADDR = 0x90000
MAILBOX_SIZE = 868
MAILBOX_MAGIC = 0x31545347  # 'GST1'

FLM_BASE = os.environ.get("FLM_URL", "http://127.0.0.1:52625")
FLM_MODEL = os.environ.get("FLM_MODEL", "gemma4-it:e4b")
CURL = "/mnt/c/Windows/System32/curl.exe"
# curl.exe (Windows プロセス) に JSON ボディを渡すための一時ファイル
WIN_TMP_WSL = "/mnt/c/Users/hight/AppData/Local/Temp/autoplay_req.json"
WIN_TMP_WIN = r"C:\Users\hight\AppData\Local\Temp\autoplay_req.json"
# スクリーンショットは emulator が Windows 側パスに書く
SHOT_WIN_DIR = r"C:\Users\hight\Documents\np21w"
SHOT_WSL_DIR = "/mnt/c/Users/hight/Documents/np21w"

ST_NAMES = {
    0: "TITLE", 1: "DICE", 2: "MOVING", 3: "BATTLE", 4: "VILLAGE",
    5: "SHOP", 6: "MESSAGE", 7: "TRANSFORM", 8: "BRANCH", 9: "COLLECT",
    10: "CASTLE", 11: "RESULT", 12: "SELL", 13: "INVENTORY", 14: "DUNGEON",
}

# 人間手番で意味のあるキー候補 (main.c の handle_key と対応)。
# W(保存)/L(ロード)/デバッグワープはシナリオ側で使うので既定候補から外す
CANDIDATES = {
    0: ["1"],
    1: ["2", "I", "M"],
    3: ["1", "2", "3", "4", "5", "0"],
    4: ["1", "3"],
    5: ["N", "1", "2", "4", "3"],
    7: ["1", "3"],
    8: ["1", "2", "3", "4"],
    9: ["N", "1", "2", "3"],
    10: ["1", "3"],
    11: ["1"],
    12: ["N", "1", "3"],
    13: ["N", "1", "3"],
    14: ["1", "3"],
}
# LLM 不調・スタック時の決定的フォールバック
DEFAULTS = {
    0: "1", 1: "2", 3: "1", 4: "1", 5: "3", 7: "3", 8: "1", 9: "3",
    10: "3", 11: "1", 12: "3", 13: "3", 14: "3",
}

# 待つだけの状態 (キーを送らない)
PASSIVE_STATES = (2, 6)


# ======================================================================
#  メールボックス
# ======================================================================

def read_mailbox(retries=6):
    """状態ブロックを読む。torn read は seq で検出してリトライ。
    magic が無ければ None (ゲームが動いていない)。"""
    for _ in range(retries):
        raw = emu.get("/api/mem?addr=0x%X&len=%d&space=phys"
                      % (MAILBOX_ADDR, MAILBOX_SIZE))
        j = json.loads(raw)
        data = bytes.fromhex(j["hex"])
        head = struct.unpack_from("<IIHHIiHBBBBBBIbbH", data, 0)
        (magic, seq_open, version, total, frame, state, turn, week,
         cur, nplayers, boss, dfloor, vtype, loot, vwinner, dice,
         _pad) = head
        if magic != MAILBOX_MAGIC:
            return None
        (db_mem,) = struct.unpack_from("<I", data, 860)
        (seq_close,) = struct.unpack_from("<I", data, 864)
        if seq_open != seq_close:
            time.sleep(0.05)
            continue
        players = []
        for i in range(4):
            off = 36 + 52 * i
            (name, hp, mhp, pos, level, flags, gold, eqc, _p, eatk, edef) = \
                struct.unpack_from("<32shhHBBIBBhh", data, off)
            players.append({
                "name": name.split(b"\0")[0].decode("utf-8", "replace"),
                "hp": hp, "max_hp": mhp, "pos": pos, "level": level,
                "cpu": bool(flags & 1), "devil": bool(flags & 2),
                "gold": gold, "equip_count": eqc,
                "eff_atk": eatk, "eff_def": edef,
            })
        line_count = data[244]
        attrs = list(data[245:250])
        lines = []
        for i in range(5):
            off = 252 + 121 * i
            s = data[off:off + 121].split(b"\0")[0]
            lines.append(s.decode("utf-8", "replace"))
        return {
            "seq": seq_open, "version": version, "frame": frame,
            "state": state, "state_name": ST_NAMES.get(state, "?%d" % state),
            "turn": turn, "week": week, "cur": cur, "nplayers": nplayers,
            "boss": boss, "dungeon_floor": dfloor, "dungeon_loot": loot,
            "victory_type": vtype, "victory_winner": vwinner, "dice": dice,
            "db_mem": db_mem,
            "players": players,
            "panel": [l for i, l in enumerate(lines) if i < line_count],
            "attrs": attrs[:line_count],
        }
    return {"torn": True}


def sanity_anomalies(s):
    """状態の妥当性チェック。おかしい点のリストを返す"""
    a = []
    if s["state"] not in ST_NAMES:
        a.append("game_state out of range: %d" % s["state"])
    for i, p in enumerate(s["players"]):
        if p["hp"] < 0 or p["hp"] > p["max_hp"]:
            a.append("player%d hp %d/%d" % (i, p["hp"], p["max_hp"]))
        if p["gold"] > 4000000000:
            a.append("player%d gold underflow? %d" % (i, p["gold"]))
        if p["max_hp"] <= 0:
            a.append("player%d max_hp %d" % (i, p["max_hp"]))
    for i, p in enumerate(s["players"]):
        if p.get("equip_count", 4) != 4:
            a.append("player%d equip_count %d (should be 4)" % (i, p["equip_count"]))
    if s["boss"] > 9:
        a.append("boss_progress %d" % s["boss"])
    return a


# ======================================================================
#  flm (ローカルLLM)
# ======================================================================

def flm_chat(messages, max_tokens=64, timeout=120):
    """OpenAI互換 /v1/chat/completions。curl.exe 経由 (WSL→Windows)。
    失敗時は None"""
    body = {
        "model": FLM_MODEL,
        "messages": messages,
        "stream": False,
        "temperature": 0.7,
        "max_tokens": max_tokens,
    }
    with open(WIN_TMP_WSL, "w", encoding="utf-8") as f:
        json.dump(body, f, ensure_ascii=False)
    try:
        p = subprocess.run(
            [CURL, "-s", "-m", str(timeout), "-X", "POST",
             FLM_BASE + "/v1/chat/completions",
             "-H", "Content-Type: application/json",
             "-d", "@" + WIN_TMP_WIN],
            capture_output=True, timeout=timeout + 10)
        if p.returncode != 0:
            return None
        j = json.loads(p.stdout.decode("utf-8", "replace"))
        text = j["choices"][0]["message"]["content"]
        # gemma は think:true。思考ブロックを剥がす
        text = re.sub(r"<think>.*?</think>", "", text, flags=re.S)
        return text.strip()
    except Exception:
        return None


SYSTEM_PROMPT = (
    "あなたはPC-98のすごろくRPGをテストプレイするAIです。"
    "毎回、画面の文字と選べるキーの一覧を渡します。"
    "次に押すキーを1文字だけ答えてください。説明は不要です。"
)


def build_user_prompt(s, cands, history, goal):
    me = s["players"][s["cur"]] if s["cur"] < len(s["players"]) else None
    lines = []
    lines.append("場面: %s / %dターン 第%d週" %
                 (s["state_name"], s["turn"], s["week"]))
    if me:
        lines.append("手番: %s Lv%d HP%d/%d 所持%dG" %
                     (me["name"], me["level"], me["hp"], me["max_hp"],
                      me["gold"]))
    lines.append("画面:")
    for l in s["panel"]:
        lines.append("  " + l)
    if history:
        lines.append("直近の行動: " + " ".join(history[-6:]))
    lines.append("目的: %s" % goal)
    lines.append("選べるキー: %s" % " ".join(cands))
    lines.append("答え (キー1文字のみ):")
    return "\n".join(lines)


def pick_key_from_reply(reply, cands):
    if not reply:
        return None
    up = reply.upper()
    for ch in up:
        if ch in cands:
            return ch
    return None


# ======================================================================
#  セッション / ログ
# ======================================================================

class Session(object):
    def __init__(self, name):
        self.dir = os.path.join(LOGS, name)
        self.shots = os.path.join(self.dir, "shots")
        os.makedirs(self.shots, exist_ok=True)
        self.dec_path = os.path.join(self.dir, "decisions.jsonl")
        self.ano_path = os.path.join(self.dir, "anomalies.jsonl")
        latest = os.path.join(LOGS, "latest")
        try:
            if os.path.islink(latest) or os.path.exists(latest):
                os.remove(latest)
            os.symlink(self.dir, latest)
        except OSError:
            pass

    def log(self, rec):
        rec["ts"] = time.strftime("%H:%M:%S")
        with open(self.dec_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
        if rec.get("anomalies"):
            with open(self.ano_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")

    def screenshot(self, tag):
        win = SHOT_WIN_DIR + "\\autoplay_shot.bmp"
        wsl = SHOT_WSL_DIR + "/autoplay_shot.bmp"
        try:
            emu.get_to_file("/api/screenshot", win)
            dst = os.path.join(self.shots, tag + ".png")
            try:
                from PIL import Image
                Image.open(wsl).save(dst)
            except Exception:
                dst = os.path.join(self.shots, tag + ".bmp")
                with open(wsl, "rb") as a, open(dst, "wb") as b:
                    b.write(a.read())
            return dst
        except Exception as e:
            return "screenshot failed: %s" % e


# ======================================================================
#  メインループ
# ======================================================================

def send_key(key):
    emu.post("/api/key", "seq=%s&hold=250" % key)


def state_sig(s):
    return (s["state"], tuple(s["panel"]), s["cur"])


def run(args):
    os.makedirs(LOGS, exist_ok=True)
    name = args.session or time.strftime("%Y%m%d_%H%M%S")
    sess = Session(name)
    with open(PIDFILE, "w") as f:
        f.write(str(os.getpid()))

    history = []
    decisions = 0
    last_frame = -1
    last_frame_t = time.time()
    last_sig = None
    same_sig_count = 0
    llm_ok = 0
    llm_fail = 0

    sess.log({"event": "start", "policy": args.policy, "goal": args.goal,
              "max_decisions": args.max_decisions})

    while decisions < args.max_decisions:
        try:
            s = read_mailbox()
        except emu.EmuError as e:
            sess.log({"event": "anomaly", "anomalies": ["emu unreachable: %s" % e]})
            time.sleep(3)
            continue

        # --- 生存確認 ---
        if s is None:
            sess.log({"event": "anomaly",
                      "anomalies": ["mailbox magic lost (game not running?)"],
                      "shot": sess.screenshot("magic_lost_%03d" % decisions)})
            time.sleep(3)
            continue
        if s.get("torn"):
            time.sleep(0.2)
            continue

        if s["frame"] != last_frame:
            last_frame = s["frame"]
            last_frame_t = time.time()
        elif time.time() - last_frame_t > 8:
            # フレームカウンタが止まった = フリーズ
            st = {}
            try:
                st = json.loads(emu.get("/api/status"))
            except Exception:
                pass
            sess.log({"event": "anomaly",
                      "anomalies": ["frame counter frozen >8s"],
                      "emu_status": st,
                      "shot": sess.screenshot("frozen_%03d" % decisions)})
            last_frame_t = time.time()  # 連打しない
            time.sleep(3)
            continue

        anomalies = sanity_anomalies(s)

        # --- CPU 手番 / 受動状態は待つ ---
        me = s["players"][s["cur"]] if s["cur"] < 4 else None
        if s["state"] in PASSIVE_STATES or (me and me["cpu"]):
            if anomalies:
                sess.log({"event": "observe", "state": s["state_name"],
                          "anomalies": anomalies, "panel": s["panel"]})
            time.sleep(args.interval)
            continue

        cands = CANDIDATES.get(s["state"])
        if not cands:
            time.sleep(args.interval)
            continue

        # バトルは結果表示中もST_BATTLEのまま。パネルが入力待ち
        # ("選べ" を含む) になるまでキーを送らない
        if s["state"] == 3 and not any("選べ" in l for l in s["panel"]):
            time.sleep(0.5)
            continue

        # --- スタック検知: 同じ画面が続いたら記録し、別の手を打つ ---
        sig = state_sig(s)
        if sig == last_sig:
            same_sig_count += 1
        else:
            same_sig_count = 0
            last_sig = sig
        forced = None
        if same_sig_count >= 8:
            anomalies.append("stuck: same screen for %d decisions"
                             % same_sig_count)
            forced = cands[same_sig_count % len(cands)]  # 総当たりで脱出
        if same_sig_count == 8:
            sess.screenshot("stuck_%03d" % decisions)

        # --- キー決定 ---
        model_raw = None
        latency = 0.0
        if forced:
            key = forced
            src = "forced"
        elif args.policy == "llm":
            t0 = time.time()
            model_raw = flm_chat([
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user",
                 "content": build_user_prompt(s, cands, history, args.goal)},
            ])
            latency = round(time.time() - t0, 2)
            key = pick_key_from_reply(model_raw, cands)
            if key:
                llm_ok += 1
                src = "llm"
            else:
                llm_fail += 1
                key = DEFAULTS.get(s["state"], cands[0])
                src = "fallback"
        else:
            key = DEFAULTS.get(s["state"], cands[0])
            src = "scripted"

        send_key(key)
        decisions += 1
        history.append("%s:%s" % (s["state_name"], key))

        rec = {
            "n": decisions, "state": s["state_name"], "turn": s["turn"],
            "week": s["week"], "cur": s["cur"],
            "panel": s["panel"], "key": key, "src": src,
            "latency": latency,
        }
        if model_raw is not None:
            rec["model_raw"] = model_raw[:200]
        if anomalies:
            rec["anomalies"] = anomalies
        if me:
            rec["me"] = {"name": me["name"], "hp": me["hp"],
                         "gold": me["gold"], "pos": me["pos"]}
        sess.log(rec)

        if decisions % 10 == 0:
            sess.screenshot("d%04d" % decisions)

        # --- 画面が変わるまで少し待つ ---
        t0 = time.time()
        while time.time() - t0 < 4:
            time.sleep(0.4)
            try:
                s2 = read_mailbox()
            except emu.EmuError:
                break
            if not s2 or s2.get("torn"):
                continue
            if state_sig(s2) != sig:
                break

    sess.log({"event": "end", "decisions": decisions,
              "llm_ok": llm_ok, "llm_fail": llm_fail,
              "shot": sess.screenshot("final")})
    try:
        os.remove(PIDFILE)
    except OSError:
        pass
    print("done: %d decisions -> %s" % (decisions, sess.dir))


# ======================================================================
#  シナリオ実行 — 決め打ちの手順を流し、状態スナップショットを記録する
#
#  シナリオは JSON: {"name": ..., "steps": [ ... ]}
#  step の種類:
#    {"wait": {"state": "DICE", "human": true, "timeout": 60}}
#        指定状態 (かつ人間手番) になるまで待つ
#    {"key": "W"}          /api/key seq=W を送る (ESC なども可)
#    {"text": "game\r"}    /api/key text=... を送る (シェル操作用)
#    {"sleep": 2}
#    {"snap": "before_save"}   メールボックス全体を記録 (後で比較する)
#    {"shot": "tag"}           スクリーンショット
# ======================================================================

def run_scenario(path, session_name):
    os.makedirs(LOGS, exist_ok=True)
    sc = json.load(open(path, encoding="utf-8"))
    sess = Session(session_name or
                   "scenario_" + time.strftime("%Y%m%d_%H%M%S"))
    sess.log({"event": "scenario_start", "name": sc.get("name"),
              "file": path})
    ok = True
    for i, step in enumerate(sc["steps"]):
        if "wait" in step:
            w = step["wait"]
            want = w.get("state")
            human = w.get("human", False)
            deadline = time.time() + w.get("timeout", 60)
            hit = False
            while time.time() < deadline:
                s = read_mailbox()
                if s and not s.get("torn"):
                    if (want is None or s["state_name"] == want):
                        me = s["players"][s["cur"]] if s["cur"] < 4 else None
                        if not human or (me and not me["cpu"]):
                            hit = True
                            break
                time.sleep(0.5)
            sess.log({"event": "wait", "step": i, "want": want,
                      "ok": hit})
            if not hit:
                sess.log({"event": "scenario_fail", "step": i,
                          "anomalies": ["wait timeout: %s" % want],
                          "shot": sess.screenshot("fail_%d" % i)})
                ok = False
                break
        elif "key" in step:
            send_key(step["key"])
            sess.log({"event": "key", "step": i, "key": step["key"]})
            time.sleep(0.5)
        elif "text" in step:
            emu.post("/api/key", "text=" + step["text"])
            sess.log({"event": "text", "step": i, "text": step["text"]})
            time.sleep(0.5)
        elif "sleep" in step:
            time.sleep(step["sleep"])
        elif "snap" in step:
            s = read_mailbox()
            sess.log({"event": "snap", "step": i, "tag": step["snap"],
                      "mailbox": s})
        elif "shot" in step:
            sess.log({"event": "shot", "step": i,
                      "path": sess.screenshot(step["shot"])})
    sess.log({"event": "scenario_end", "ok": ok})
    print("scenario %s -> %s" % ("OK" if ok else "FAILED", sess.dir))
    return 0 if ok else 1


# ======================================================================
#  CLI
# ======================================================================

def cmd_status():
    alive = False
    if os.path.exists(PIDFILE):
        pid = int(open(PIDFILE).read().strip())
        try:
            os.kill(pid, 0)
            alive = True
        except OSError:
            pass
    latest = os.path.join(LOGS, "latest", "decisions.jsonl")
    lastline = None
    count = 0
    if os.path.exists(latest):
        with open(latest, encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    lastline = line.strip()
                    count += 1
    print(json.dumps({"running": alive, "decisions_logged": count,
                      "last": json.loads(lastline) if lastline else None},
                     ensure_ascii=False, indent=1))


def cmd_tail(n):
    latest = os.path.join(LOGS, "latest", "decisions.jsonl")
    if not os.path.exists(latest):
        print("no log")
        return
    lines = [l for l in open(latest, encoding="utf-8") if l.strip()]
    for l in lines[-n:]:
        print(l.rstrip())


def cmd_stop():
    if not os.path.exists(PIDFILE):
        print("not running")
        return
    pid = int(open(PIDFILE).read().strip())
    try:
        os.kill(pid, signal.SIGTERM)
        print("stopped pid %d" % pid)
    except OSError as e:
        print("kill failed: %s" % e)
    try:
        os.remove(PIDFILE)
    except OSError:
        pass


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("start")
    st.add_argument("--policy", choices=["llm", "scripted"], default="llm")
    st.add_argument("--max-decisions", type=int, default=50)
    st.add_argument("--interval", type=float, default=1.0)
    st.add_argument("--goal", default="いろいろな行動を試してバグを探す。同じ行動の繰り返しは避ける")
    st.add_argument("--session", default=None)
    sc = sub.add_parser("scenario")
    sc.add_argument("file")
    sc.add_argument("--session", default=None)
    tl = sub.add_parser("tail")
    tl.add_argument("-n", type=int, default=10)
    sub.add_parser("status")
    sub.add_parser("stop")
    args = ap.parse_args()

    if args.cmd == "start":
        run(args)
    elif args.cmd == "scenario":
        sys.exit(run_scenario(args.file, args.session))
    elif args.cmd == "status":
        cmd_status()
    elif args.cmd == "tail":
        cmd_tail(args.n)
    elif args.cmd == "stop":
        cmd_stop()


if __name__ == "__main__":
    main()
