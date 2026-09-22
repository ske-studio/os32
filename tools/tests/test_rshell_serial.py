"""rshell_serial.py の「応答の識別」のホスト試験。

記録: tools/tests/serial_vfast_tdd.md
票  : docs/tasks/realhw/TASK_SERIAL_VFAST.md (Codex レビュー往復 3 ⑤⑥)

実物の tools/rshell_serial.py を import して、純粋な判定だけを回す
(pyserial もシリアルポートも要らない — モジュールは pyserial が無くても
import できるようにしてある)。

**ここはエミュレータでも実機でも「たまたま通る」**。EOT の取り違えは
タイミングで起きるので、再現を待っていては直したかどうか分からない。
だから識別の規則そのものをここで固定する。

  python3 -B tools/tests/test_rshell_serial.py            # 全ケース
  python3 -B tools/tests/test_rshell_serial.py --mutate   # 否定側
"""
import importlib.util
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "tools/rshell_serial.py"


def load(path=None):
    """実物 (または変異させた写し) を import して返す。"""
    spec = importlib.util.spec_from_file_location(
        "rshell_serial_under_test", str(path or SRC))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


FAILED = []


def check(cond, what):
    if not cond:
        FAILED.append(what)


# ---------------------------------------------------------------------------
#  (a) エコー行は **行全体** で比べる (⑥)
# ---------------------------------------------------------------------------
def case_echo(r):
    # 素直な成功。
    check(r.check_echo("> ver\nOS32 v1.4\n  Build: Sep 22 2026\n", "ver"),
          "echo: plain match")
    # 先頭の改行は読み飛ばす (前のコマンドの残りの改行)。
    check(r.check_echo("\r\n> ver\nOS32\n", "ver"), "echo: leading newlines")
    check(r.check_echo("> ver\r\nOS32\n", "ver"), "echo: CRLF")

    # **前方一致では通ってしまう組み合わせ。** `startswith("> ver")` だと
    # `> version` も `> ver foo` も通る = 別のコマンドの応答を読んでいるのに
    # 気づけない (⑥)。
    check(not r.check_echo("> version\nOS32\n", "ver"),
          "echo: '> version' is NOT an echo of 'ver'")
    check(not r.check_echo("> ver foo\nOS32\n", "ver"),
          "echo: '> ver foo' is NOT an echo of 'ver'")
    check(not r.check_echo("> verbose\n", "ver"), "echo: '> verbose' rejected")

    # 別のコマンドの応答 (1 つずれている)。
    check(not r.check_echo("> serial 115200\nRS-232C init\n", "ver"),
          "echo: previous command's reply rejected")
    # エコーが無い (EOT だけ拾った)。
    check(not r.check_echo("  Build: Sep 22 2026\n", "ver"),
          "echo: body without echo rejected")
    check(not r.check_echo("", "ver"), "echo: empty rejected")

    # **`exit` はエコーを出さない** — ゲストは kprintf より先に break する
    # (userland/shell/rshell.c)。ここを desync と誤判定していた (⑥)。
    check(r.check_echo("", "exit"), "echo: exit needs no echo")
    check(r.check_echo("anything at all\n", "exit"), "echo: exit always ok")
    check("exit" in r.NO_ECHO_CMDS, "echo: exit is listed as echo-less")

    # 引数つきのコマンドも行全体で一致すること。
    check(r.check_echo("> serial 9600\nRS-232C init\n", "serial 9600"),
          "echo: command with args")
    check(not r.check_echo("> serial 9600 extra\n", "serial 9600"),
          "echo: extra args rejected")


# ---------------------------------------------------------------------------
#  (b) `ver` の応答は本文 + エコーで識別する (⑤)
# ---------------------------------------------------------------------------
def case_probe(r):
    good = "> ver\nOS32 v1.4\n  Build: Sep 22 2026 12:00:00\n"

    check(r.probe_ok(good, True), "probe: real ver reply")

    # EOT が来ていない = 速度が合っていない。
    check(not r.probe_ok(good, False), "probe: no EOT is a failure")

    # **先行コマンド (`serial N`) の EOT を拾っただけ** — 本文もエコーも無い。
    # これを成功と読むと、以後の応答が 1 コマンドずれる (⑤)。
    check(not r.probe_ok("", True), "probe: bare EOT rejected")
    check(not r.probe_ok("\r\n", True), "probe: whitespace-only rejected")

    # 切替コマンドの応答が遅れて届いた場合。EOT はあるが `ver` ではない。
    check(not r.probe_ok("> serial 115200\nRS-232C init: requested 115200bps\n",
                         True),
          "probe: delayed switch reply rejected")

    # 本文はあるがエコーが別物 (1 つずれている)。
    check(not r.probe_ok("> serial 9600\n  Build: Sep 22 2026\n", True),
          "probe: right body but wrong echo rejected")

    # エコーはあるが本文が無い (化けて Build: が落ちた)。
    check(not r.probe_ok("> ver\n\xff\xfe\n", True),
          "probe: right echo but no body rejected")

    # 本文の目印が `ver` にしか出ないこと (userland/shell/cmd_base.c)。
    check(r.PROBE_EXPECT == "Build:", "probe: body marker is 'Build:'")
    check(r.PROBE_CMD == "ver", "probe: probe command is 'ver'")


# ---------------------------------------------------------------------------
#  (c) 切替の応答は **旧速度で** 待ち切る (⑤)
# ---------------------------------------------------------------------------
def case_switch(r):
    # ゲストは切替の **前に** エコーを出すので、旧速度で読める。
    check(r.switch_reply_ok("> serial 115200\nRS-232C init: requested\n",
                            115200),
          "switch: echo at the old speed")
    check(r.switch_reply_ok("\r\n> serial 38400\n", 38400),
          "switch: echo with leading newlines")

    # 速度が違えばエコーも違う。
    check(not r.switch_reply_ok("> serial 38400\n", 115200),
          "switch: echo for another baud rejected")
    # 何も読めなかった (ゲストが切替行を受け取っていない)。
    check(not r.switch_reply_ok("", 115200), "switch: silence rejected")
    # 化けたバイトだけ。
    check(not r.switch_reply_ok("\xff\xfe\xfd", 115200),
          "switch: garbage rejected")

    # **旧速度で待つ上限は新速度の確認より長い** — ここで待ち切らないと
    # 遅れた切替の EOT を新速度で拾う。
    check(r.SWITCH_REPLY_S >= r.SWITCH_PROBE_TIMEOUT_S,
          "switch: old-speed wait is not shorter than the probe")
    check(r.SWITCH_REPLY_S == 5.0, "switch: old-speed wait is 5s")
    # 失敗のあとゲストの番犬 (500 tick = 5 秒) が戻すのを待つ。
    check(r.WATCHDOG_WAIT_S > 5.0, "switch: waits past the guest watchdog")


# ---------------------------------------------------------------------------
#  (d) --fast が受ける速度は資料の表と同じ
# ---------------------------------------------------------------------------
def case_bauds(r):
    check(r.FAST_BAUDS == (9600, 14400, 19200, 28800, 38400, 57600, 115200),
          "bauds: matches the V-FAST table in drivers/serial_plan.c")


CASES = {
    "echo": case_echo,
    "probe": case_probe,
    "switch": case_switch,
    "bauds": case_bauds,
}

# 否定側。実装を 1 か所だけ壊して RED になることを見る。
MUTATIONS = [
    (r"    return first == echo_line\(cmd\)",
     "    return first.startswith(echo_line(cmd))",
     "エコーを前方一致で見る (`> version` を `ver` の応答と読む — ⑥)"),
    (r"    if cmd in NO_ECHO_CMDS:\n        return True\n",
     "",
     "`exit` の例外を外す (エコーの無い `exit` を desync と誤判定 — ⑥)"),
    (r"    if PROBE_EXPECT not in text:\n        return False\n",
     "",
     "本文を見ない (先行コマンドの EOT を `ver` の成功と読む — ⑤)"),
    (r"    return check_echo\(text, PROBE_CMD\)",
     "    return True",
     "`ver` のエコーを見ない (1 つずれた応答を成功と読む — ⑤)"),
    (r"    if not eot:\n        return False\n",
     "",
     "EOT が来なくても成功と読む (速度不一致を見逃す)"),
    (r'    return check_echo\(text, "serial %d" % baud\)',
     "    return True",
     "切替の応答を確かめずに新速度へ移る (遅れた EOT を `ver` と取り違える — ⑤)"),
    (r"SWITCH_REPLY_S = 5\.0", "SWITCH_REPLY_S = 0.0",
     "旧速度で待たない (遅れた切替 EOT を新速度で拾う — ⑤)"),
]


def run(mod, names):
    global FAILED
    bad = 0
    for name in names:
        FAILED = []
        CASES[name](mod)
        rc = 1 if FAILED else 0
        for f in FAILED:
            print(f"  FAIL {name}: {f}", flush=True)
        print(f"EXIT {name}={rc}", flush=True)
        bad += rc
    print(f"SUMMARY {len(names) - bad}/{len(names)} PASS", flush=True)
    return bad


def mutate(tmp):
    original = SRC.read_text(encoding="utf-8")
    bad = 0
    for i, (pattern, repl, why) in enumerate(MUTATIONS, 1):
        text, n = re.subn(pattern, repl, original, count=1)
        if n != 1:
            print(f"MUTATION {i} NOT APPLICABLE: {why}", flush=True)
            bad += 1
            continue
        # **実物は書き換えない** — 写しの上で変異させる (check-par で並列可)。
        path = pathlib.Path(tmp) / ("mut%d.py" % i)
        path.write_text(text, encoding="utf-8")
        try:
            mod = load(path)
        except Exception:
            print(f"MUTATION {i} RED (import): {why}", flush=True)
            continue
        hits = 0
        for name in CASES:
            global FAILED
            FAILED = []
            try:
                CASES[name](mod)
            except Exception:
                FAILED.append("raised")
            hits += 1 if FAILED else 0
        status = "RED" if hits else "**GREEN (見逃し)**"
        print(f"MUTATION {i} {status} ({hits} 件): {why}", flush=True)
        bad += not hits
    return bad


if __name__ == "__main__":
    args = sys.argv[1:]
    names = [a for a in args if not a.startswith("--")] or list(CASES)
    mod = load()
    print("HOST import PASS (real tools/rshell_serial.py)", flush=True)
    rc = run(mod, names)
    if "--mutate" in args:
        with tempfile.TemporaryDirectory(prefix="os32-rshell-serial-") as tmp:
            rc += mutate(tmp)
    sys.exit(bool(rc))
