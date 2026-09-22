#!/usr/bin/env python3
"""rshell_serial.py — 実機の OS32 リモートシェル (rshell) にシリアルで話す。

ゲスト側 (userland/shell/rshell.c) の約束:
  - 9600bps 8N1 (drivers/serial.c の既定)。
  - ホストは 1 行 (コマンド + '\\n') を送り、ゲストは応答の最後に EOT (0x04) を返す。
  - rshell に入った瞬間にも EOT を 1 つ送る (同期用)。
NP21/W の aidebug が /api/cmd でやっていること (aidebug_api.cpp、timeout 15s) と同じ。

Windows 側の Python (pyserial 入り) で動かす:
  C:\\WATCOM\\.venv\\Scripts\\python.exe \\\\wsl.localhost\\Ubuntu\\home\\hight\\os32\\tools\\rshell_serial.py --port COM3 cmd ver
  ... --port COM3 repl          # 対話 (exit / Ctrl-C で終了。'exit' はゲストの rshell も閉じる)
  ... --port COM3 sync          # 溜まっている受信を捨てて EOT を待つだけ
"""
import argparse
import sys
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    sys.stderr.write("pyserial が要る: python -m pip install pyserial\n")
    sys.exit(2)

EOT = b"\x04"


def open_port(name, baud):
    return serial.Serial(name, baudrate=baud, bytesize=8, parity="N",
                         stopbits=1, timeout=0.2)


def read_until_eot(port, timeout_s):
    """EOT が来るまで読む。戻り値 (本文 bytes, EOT を見たか)。"""
    buf = bytearray()
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        chunk = port.read(256)
        if chunk:
            i = chunk.find(EOT)
            if i >= 0:
                buf += chunk[:i]
                return bytes(buf), True
            buf += chunk
    return bytes(buf), False


def send_cmd(port, line, timeout_s):
    port.reset_input_buffer()
    port.write(line.encode("utf-8") + b"\n")
    port.flush()
    body, ok = read_until_eot(port, timeout_s)
    return body.decode("utf-8", errors="replace"), ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="COM3 など")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--timeout", type=float, default=15.0,
                    help="EOT を待つ秒数 ([V3]: 15 以上、長いコマンドは 60+)")
    sub = ap.add_subparsers(dest="mode", required=True)
    p_cmd = sub.add_parser("cmd", help="1 コマンドを送って応答を出す")
    p_cmd.add_argument("line", nargs="+")
    sub.add_parser("repl", help="対話")
    sub.add_parser("sync", help="受信を捨てて EOT を 1 つ待つ")
    args = ap.parse_args()

    port = open_port(args.port, args.baud)
    try:
        if args.mode == "sync":
            port.reset_input_buffer()
            body, ok = read_until_eot(port, args.timeout)
            sys.stdout.write(body.decode("utf-8", errors="replace"))
            print("\n[sync] EOT %s" % ("ok" if ok else "TIMEOUT"))
            return 0 if ok else 1
        if args.mode == "cmd":
            text, ok = send_cmd(port, " ".join(args.line), args.timeout)
            sys.stdout.write(text)
            if not ok:
                print("\n[rshell_serial] timeout waiting for EOT (%.0fs)" % args.timeout)
                return 1
            return 0
        # repl
        print("[rshell_serial] %s %dbps — 'exit' でゲストの rshell も閉じる" % (args.port, args.baud))
        while True:
            try:
                line = input("os32> ")
            except (EOFError, KeyboardInterrupt):
                print()
                return 0
            if not line.strip():
                continue
            text, ok = send_cmd(port, line, args.timeout)
            sys.stdout.write(text)
            if not text.endswith("\n"):
                print()
            if not ok:
                print("[rshell_serial] timeout waiting for EOT")
            if line.strip() == "exit":
                return 0
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
