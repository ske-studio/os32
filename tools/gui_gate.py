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
    if text is not None:
        post("/api/key", {"text": text})
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


def enter_gshell():
    key(text="os32gui")
    key(seq="RETURN")
    time.sleep(6)


def leave_gshell(mouse):
    """デバッグ用の ESC (v1.2 完成で撤去予定) で CUI へ戻り、rshell を復旧する。"""
    key(seq="ESC")
    time.sleep(5)
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
    print("[v11] enter gshell + gui_demo (F1)")
    enter_gshell()
    st = status()
    print("  status %s" % st)
    key(seq="F1")
    time.sleep(4)
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
    print("[v11] checkbox (257,252) / OK (500,324) on moved Widgets")
    m.click(257, 252)
    m.click(500, 324)
    shots.take("v11_5_widgets_clicked")
    print("[v11] close Help via x (604,90)")
    m.click(604, 90)
    shots.take("v11_6_help_closed")
    print("[v11] bottom edge (20,%d)" % (h - 20))
    m.move(20, h - 20)
    shots.take("v11_7_bottom_edge")
    print("[v11] ESC closes demo, then CUI")
    key(seq="ESC")
    time.sleep(2)
    ok = leave_gshell(m)
    st2 = status()
    print("  status %s" % st2)
    return ok and st2.get("wab_relay") == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario", choices=["v11", "shot", "click"])
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
    ok = scenario_v11(a.h, shots)
    print("RESULT: %s (判定はスクリーンショットで)" % ("OK" if ok else "NG"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
