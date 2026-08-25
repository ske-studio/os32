#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""flm (FastFlowLM) サーバの起動・停止・状態確認

flm は Windows 専用で `flm run` は対話 TUI のため自動化に使えない。
`flm serve <model>` の OpenAI 互換 API (127.0.0.1:52625) を使う。
WSL からは Windows の curl.exe 経由で 127.0.0.1 に届く
(np21w_client.py と同じ手法。ファイアウォール設定は不要)。

使い方:
  python3 flm_serve.py start [--model gemma4-it:e4b] [--wait 240]
  python3 flm_serve.py status
  python3 flm_serve.py stop
"""

import argparse
import json
import subprocess
import sys
import time

CURL = "/mnt/c/Windows/System32/curl.exe"
BASE = "http://127.0.0.1:52625"
DEFAULT_MODEL = "gemma4-it:e4b"


def models(timeout=8):
    """/v1/models を叩く。届かなければ None"""
    try:
        p = subprocess.run([CURL, "-s", "-m", str(timeout), BASE + "/v1/models"],
                           capture_output=True, timeout=timeout + 5)
        if p.returncode != 0 or not p.stdout:
            return None
        return json.loads(p.stdout.decode("utf-8", "replace"))
    except Exception:
        return None


def start(model, wait):
    if models():
        print("already running")
        return 0
    # デタッチ起動。Popen + DEVNULL でパイプを渡さない
    # (run() で待つと flm が親コンソールを掴んで返ってこない)
    subprocess.Popen(
        ["cmd.exe", "/c", "start", "", "/min", "flm", "serve", model],
        cwd="/mnt/c/", stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("starting flm serve %s (モデルロードに数十秒かかる)..." % model)
    t0 = time.time()
    while time.time() - t0 < wait:
        time.sleep(5)
        m = models()
        if m:
            print("up after %ds: %s" % (int(time.time() - t0),
                                        json.dumps(m)[:200]))
            return 0
    print("ERROR: server did not come up within %ds" % wait)
    return 1


def stop():
    subprocess.run(["taskkill.exe", "/IM", "flm.exe", "/F"], check=False)
    print("stopped")


def status():
    m = models()
    print(json.dumps({"running": bool(m), "models": m}, ensure_ascii=False))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("start")
    st.add_argument("--model", default=DEFAULT_MODEL)
    st.add_argument("--wait", type=int, default=240)
    sub.add_parser("status")
    sub.add_parser("stop")
    args = ap.parse_args()
    if args.cmd == "start":
        sys.exit(start(args.model, args.wait))
    elif args.cmd == "stop":
        stop()
    else:
        status()


if __name__ == "__main__":
    main()
