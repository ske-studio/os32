#!/usr/bin/env python3
"""gui_gate.py — NP21/W ai-debug の HTTP API で GUI のゲート操作列を回す (PM 所有)。

前提: NP21/W (ai-debug 版) が 127.0.0.1:8025 で動き、OS32 が CUI (rshell) で起動済み。
マウスは `/api/mouse` の **ax/ay (シームレス絶対座標)** を使う (OS32 は NP21/W では
np2sysp getmpos で位置を取るので dx/dy は効かない。POLICY_DEBUG §4-23)。

使い方:
  python3 tools/gui_gate.py v11            # v1.1 回帰: gui_demo のドラッグ / 重なり / クリック
  python3 tools/gui_gate.py v11 --h 400    # 9801 (400 ライン) で
  python3 tools/gui_gate.py shot NAME      # 今の画面を NAME.png に保存するだけ
  python3 tools/gui_gate.py click X Y      # 1 回クリック (デバッグ)

出力: 各手順の要点 1 行と、スクリーンショット (--out、既定 build/out/gui_gate/) の PNG。
判定は人間 (PM) がする。数値 (wab_relay / scrn_ymax / fault_generation) だけ自動で照合する。
v1.2 の台本 (Start / taskbar / dialog / filer / session) は W3〜C5 の結合時にここへ足す。

CUI へ戻る経路について (G5 で ESC の即時切替と上部バーを撤去した。契約 S6 / 票 W3 §4.1):
  どの台本も Start → "CUI mode" → 確認ダイアログ Yes で戻る (`leave_gshell`)。この経路は
  `/etc/system.cfg` の `GUI=0` を**永続化する**が、台本は必ず CUI (rshell) から始まり、
  GUI へは `os32gui` コマンドで入る (`enter_gshell`) ので支障は無い。`os32gui` は cfg の
  値に関わらず GUI へ入り、次回の自動起動だけが CUI になる。GUI 自動起動へ戻したい
  ときは、ゲート後に CUI で `os32gui` を使うか cfg の `GUI=1` を書き戻すこと。
"""
import argparse
import os
import sys
import time
import urllib.parse
import urllib.request

BASE = "http://127.0.0.1:8025"


def post(path, data, timeout=20):
    body = urllib.parse.urlencode(data).encode()
    req = urllib.request.Request(BASE + path, data=body, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def get(path, timeout=20):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read(), dict(r.headers)


def key(seq=None, text=None):
    """文字列は 4 文字ずつ送る。raw リングは 32 エントリ (make+break で 1 文字 2 本) しか
    無く、長い text を一度に注入すると後ろが落ちる (2026-09-06: Run... のパスが
    `/usr/bin/gui_dem` で切れた)。8 文字 / 0.3 秒でも 9801 (planar) でアプリ実行中は
    WM の drain が追いつかず 2 文字落ちた (2026-09-07: `v12_api_test.n`) ので 4 文字に。"""
    if text is not None:
        i = 0
        while i < len(text):
            post("/api/key", {"text": text[i:i + 4]})
            time.sleep(0.35)
            i += 4
    if seq is not None:
        post("/api/key", {"seq": seq})   # urlencode が + を %2B にする


def cmd(line, timeout=60):
    return _cmd_raw(line, timeout)


def _cmd_raw(line, timeout):
    req = urllib.request.Request(BASE + "/api/cmd", data=line.encode(), method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def status(keys=("scrn_ymax", "grph_disp", "wab_relay", "fault_generation")):
    import json
    d = json.loads(get("/api/status")[0])
    return {k: d.get(k) for k in keys}


class Mouse:
    def __init__(self, h):
        self.h = h

    def move(self, px, py, settle=0.4):
        ax = (px * 65535 + 319) // 639
        ay = (py * 65535 + (self.h - 1) // 2) // (self.h - 1)
        post("/api/mouse", {"ax": ax, "ay": ay})
        time.sleep(settle)

    def press(self, btn=1):
        post("/api/mouse", {"btn": btn})
        time.sleep(0.4)

    def release(self):
        post("/api/mouse", {"btn": 0})
        time.sleep(0.6)

    def click(self, px, py, btn=1):
        self.move(px, py)
        self.press(btn)
        self.release()

    def drag(self, x0, y0, x1, y1, mid=None):
        self.move(x0, y0)
        self.press()
        for (mx, my) in (mid or []):
            self.move(mx, my)
        self.move(x1, y1)
        self.release()

    def off(self):
        post("/api/mouse", {"abs": "off"})


class Shots:
    def __init__(self, outdir):
        self.outdir = outdir
        os.makedirs(outdir, exist_ok=True)

    def take(self, name):
        data, hdr = get("/api/screenshot?src=auto")
        bmp = os.path.join(self.outdir, name + ".bmp")
        with open(bmp, "wb") as f:
            f.write(data)
        png = os.path.join(self.outdir, name + ".png")
        try:
            from PIL import Image
            Image.open(bmp).convert("RGB").save(png)
            os.remove(bmp)
            out = png
        except Exception:
            out = bmp
        print("  shot %-24s src=%s" % (name, hdr.get("X-Screen-Source", "?")))
        return out


# ---------------------------------------------------------------------------
#  v1.2 の座標 (W3 が報告した値。ax/ay 換算は Mouse が行う)
#  taskbar: Start (30,H-12)、窓ボタン #n (110+100n,H-12)、時計 (614,H-12)
#  Start menu 行 r: (82, H-107+18r) = Programs / File Manager / Run... / CUI mode / Shut Down
#  確認ダイアログ Yes (410, H/2+11) / No (494, H/2+11)、Run... の OK (360, H/2+23)
# ---------------------------------------------------------------------------
def tb(h):
    return h - 12


def start_row(h, r):
    return (82, h - 107 + 18 * r)


def enter_gshell():
    key(text="os32gui")
    key(seq="RETURN")
    time.sleep(6)


def run_dialog(mouse, path):
    """Start → "Run..." (行 2) にパスを打って RETURN。アプリが立ち上がるまで待つ。"""
    mouse.click(30, tb(mouse.h))
    mouse.click(*start_row(mouse.h, 2))
    time.sleep(1.5)
    key(text=path)
    time.sleep(1)
    key(seq="RETURN")
    time.sleep(5)


def leave_gshell(mouse, shots=None, shot_name=None):
    """Start → "CUI mode" (行 3) → 確認ダイアログ Yes で CUI へ戻り、rshell を復旧する。

    G5 で ESC の即時切替は製品から撤去したので、**これが唯一の CUI 復帰経路**
    (契約 S6 / 票 W3 §4.1〜4.2)。`shots` と `shot_name` を渡すと、Yes を押す前の
    確認ダイアログを撮る。

    この経路は `/etc/system.cfg` の `GUI=0` を永続化する。台本は CUI から始めて
    `os32gui` で GUI へ入る前提なので、それで構わない (モジュール先頭の注記を参照)。
    """
    mouse.click(30, tb(mouse.h))
    mouse.click(*start_row(mouse.h, 3))
    time.sleep(1.5)
    if shots is not None and shot_name:
        shots.take(shot_name)
    mouse.click(410, mouse.h // 2 + 11)
    time.sleep(6)
    mouse.off()
    key(text="rshell")
    key(seq="RETURN")
    time.sleep(2)
    out = _cmd_raw("ver", 20)
    ok = "OS32" in out
    print("  CUI back: %s" % ("ok" if ok else "NG (rshell?)"))
    return ok


def scenario_v11(h, shots):
    """v1.1 G2 相当: gui_demo の窓 2 枚でドラッグ / 重なり / クリック配送。"""
    m = Mouse(h)
    print("[v11] enter gshell + gui_demo (Start -> Run...)")
    enter_gshell()
    st = status()
    print("  status %s" % st)
    run_dialog(m, "/usr/bin/gui_demo.bin")
    shots.take("v11_1_two_windows")
    print("[v11] drag Widgets title (100,58) -> (300,208) with XOR frame")
    m.move(100, 58)
    m.press()
    m.move(200, 120)
    m.move(300, 208)
    shots.take("v11_2_drag_frame")
    m.release()
    shots.take("v11_3_dropped_overlap")
    print("[v11] click Help title (450,90) -> raise")
    m.click(450, 90)
    shots.take("v11_4_help_raised")
    # drop 後の Widgets は (240,198) 付近 (drag の差分 +150 を 48 に足した位置)。
    # チェックボックスは窓原点 +(17,63)、OK は +(260,134)。
    print("[v11] checkbox (257,261) / OK (500,332) on moved Widgets")
    m.click(257, 261)
    m.click(500, 332)
    shots.take("v11_5_widgets_clicked")
    print("[v11] close Help via x (604,90)")
    m.click(604, 90)
    shots.take("v11_6_help_closed")
    print("[v11] bottom edge (20,%d)" % (h - 20))
    m.move(20, h - 20)
    shots.take("v11_7_bottom_edge")
    # ESC は gui_demo 自身の終了キー (アプリが処理する)。gshell は ESC を横取り
    # しなくなった (G5) ので、デスクトップへ戻った後は Start 経由で CUI へ抜ける。
    print("[v11] ESC closes demo (app's own quit key), then CUI via Start")
    key(seq="ESC")
    time.sleep(2)
    ok = leave_gshell(m)
    st2 = status()
    print("  status %s" % st2)
    return ok and st2.get("wab_relay") == 0


def scenario_v12_g1(h, shots):
    """G1: taskbar / Start / Programs / context menu / clock / window button."""
    m = Mouse(h)
    print("[g1] enter gshell")
    enter_gshell()
    shots.take("g1_1_desktop_taskbar")
    print("[g1] Start menu open")
    m.click(30, tb(h))
    shots.take("g1_2_start_menu")
    print("[g1] Programs page")
    m.click(*start_row(h, 0))
    time.sleep(1.5)
    shots.take("g1_3_programs")
    key(seq="ESC")
    time.sleep(0.4)
    key(seq="ESC")
    time.sleep(0.4)
    print("[g1] desktop context menu (right button)")
    m.click(320, 240, btn=2)
    shots.take("g1_4_context_menu")
    key(seq="ESC")
    time.sleep(0.4)
    print("[g1] Run... -> /usr/bin/gui_demo.bin")
    run_dialog(m, "/usr/bin/gui_demo.bin")
    shots.take("g1_5_demo_with_taskbar")
    print("[g1] taskbar window button #1 -> raise Help")
    m.click(210, tb(h))
    shots.take("g1_6_window_button")
    print("[g1] wait for clock minute change (up to 65 s)")
    t0 = time.time()
    a = shots.take("g1_7_clock_a")
    while time.time() - t0 < 65:
        time.sleep(5)
    shots.take("g1_8_clock_b")
    print("[g1] quit demo (ESC = app's own quit key) and back to CUI via Start")
    key(seq="ESC")
    time.sleep(2)
    return leave_gshell(m)


def scenario_v12_g4(h, shots, do_halt=False):
    """G4: app replacement (Run... while an app runs), CUI mode with confirmation,
    optionally Shut Down (halt: NP21/W must be restarted afterwards)."""
    m = Mouse(h)
    print("[g4] gshell + gui_demo via Run...")
    enter_gshell()
    run_dialog(m, "/usr/bin/gui_demo.bin")
    shots.take("g4_1_demo")
    print("[g4] Run... again while demo runs -> v12_api_test replaces it")
    run_dialog(m, "/usr/bin/v12_api_test.bin")
    time.sleep(1)
    shots.take("g4_2_replaced")
    print("[g4] app-initiated launch (key 4 = session_launch gui_demo)")
    key(text="4")
    time.sleep(6)
    shots.take("g4_3_app_launch")
    print("[g4] CUI mode -> confirmation -> Yes")
    ok = leave_gshell(m, shots, "g4_4_cui_confirm")
    cfg = _cmd_raw("cat /etc/system.cfg", 20)
    print("  system.cfg: %s" % " | ".join(l for l in cfg.splitlines() if "GUI" in l))
    if do_halt:
        print("[g4] Shut Down -> halt (NP21/W must be restarted by os32-cycle deploy)")
        enter_gshell()
        m.click(30, tb(h))
        m.click(*start_row(h, 4))
        time.sleep(1.5)
        m.click(410, h // 2 + 11)
        time.sleep(4)
        shots.take("g4_5_halt_screen")
        print("  status %s" % status())
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario", choices=["v11", "v12g1", "v12g4", "shot", "click"])
    ap.add_argument("--halt", action="store_true", help="v12g4: 最後に Shut Down まで行う")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--h", type=int, default=480, help="画面高 (9801=400, PEGC/Cirrus=480)")
    ap.add_argument("--out", default="build/out/gui_gate")
    a = ap.parse_args()
    shots = Shots(a.out)
    if a.scenario == "shot":
        shots.take(a.args[0] if a.args else "shot")
        return 0
    if a.scenario == "click":
        Mouse(a.h).click(int(a.args[0]), int(a.args[1]))
        return 0
    if a.scenario == "v12g1":
        ok = scenario_v12_g1(a.h, shots)
    elif a.scenario == "v12g4":
        ok = scenario_v12_g4(a.h, shots, a.halt)
    else:
        ok = scenario_v11(a.h, shots)
    print("RESULT: %s (判定はスクリーンショットで)" % ("OK" if ok else "NG"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
