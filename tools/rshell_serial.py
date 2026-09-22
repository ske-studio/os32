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

--fast N で **繋いだあとに速度を上げる** (票 docs/tasks/realhw/TASK_SERIAL_VFAST.md):
  ... --port COM3 --fast 115200 cmd hexdump /bin/cfg.bin
ゲストは 9600 の互換モードで起動するので、
  1. --baud (既定 9600) で開いて `serial N` を送る
  2. **応答は待たない** — その行を書いている途中で速度が変わるので必ず化ける
  3. ポートを閉じて N で開き直し、`sync` で EOT を待って足並みを揃える
  4. 以後のコマンドは N で送る
N が 9600 なら切り替えは行わない。戻すときはゲストに `serial 9600` を送る
(--fast 9600 ではなく、N で開いた状態から普通に `cmd serial 9600`)。
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

# `serial N` を送ってから閉じるまでの間合い [秒]。9600 で 14 文字 ≒ 15ms なので
# 十分な余裕。短くすると行の途中でポートを閉じてゲストが切り替えを始めない。
SPEED_SWITCH_SETTLE_S = 0.5
# --fast が受ける速度 (drivers/serial_plan.c の表と同じ。V･FAST に入れるのは
# FIFO 搭載機だけで、入れなければゲストは互換モードのまま = 速度が合わなくなる)。
FAST_BAUDS = (9600, 14400, 19200, 28800, 38400, 57600, 115200)


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


def switch_speed(port_name, open_baud, fast_baud, timeout_s):
    """ゲストを fast_baud へ切り替えて、その速度で開いたポートを返す。

    **応答は待たない。** ゲストは `serial N` の途中で 013Ah を書き換えるので、
    そこから先のバイトは 9600 側では化ける。EOT も化けて届かないことがあるため、
    書いて掃けるのを待ったらすぐ閉じ、N で開き直して `sync` で足並みを揃える。
    """
    port = open_port(port_name, open_baud)
    try:
        port.reset_input_buffer()
        port.write(("serial %d\n" % fast_baud).encode("ascii"))
        port.flush()
        # 送信 FIFO が掃けて、ゲストが 013Ah を書き終えるまでの間合い。
        # 9600 で 1 文字 ≒ 1.04ms、行は 14 文字。余裕を見て 0.5 秒待つ。
        time.sleep(SPEED_SWITCH_SETTLE_S)
    finally:
        port.close()

    port = open_port(port_name, fast_baud)
    # 切り替え中に化けたバイトを捨てて、rshell が返す EOT を 1 つ拾う。
    # **EOT が来なくても致命ではない** (切り替えの行の EOT は化けて消えている
    # ことがある)。呼び手が次のコマンドを送れば改めて EOT が来る。
    port.reset_input_buffer()
    body, ok = read_until_eot(port, timeout_s)
    return port, body.decode("utf-8", errors="replace"), ok


def main():
    # Windows のコンソール (cp932) でも化けた応答で落ちないようにする
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="COM3 など")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--timeout", type=float, default=15.0,
                    help="EOT を待つ秒数 ([V3]: 15 以上、長いコマンドは 60+)")
    ap.add_argument("--fast", type=int, default=0, metavar="N",
                    help="繋いだあと `serial N` でゲストを N bps へ上げてから"
                         " cmd / repl を回す (%s)"
                         % "/".join(str(b) for b in FAST_BAUDS))
    sub = ap.add_subparsers(dest="mode", required=True)
    p_cmd = sub.add_parser("cmd", help="1 コマンドを送って応答を出す")
    p_cmd.add_argument("line", nargs="+")
    sub.add_parser("repl", help="対話")
    sub.add_parser("sync", help="受信を捨てて EOT を 1 つ待つ")
    args = ap.parse_args()

    baud = args.baud
    if args.fast and args.fast != args.baud:
        if args.fast not in FAST_BAUDS:
            sys.stderr.write(
                "--fast は %s のいずれか (資料 io_rs.md の V･FAST 表)\n"
                % ", ".join(str(b) for b in FAST_BAUDS))
            return 2
        if args.mode == "sync":
            sys.stderr.write("--fast は cmd / repl で使う (sync は切り替えない)\n")
            return 2
        port, body, ok = switch_speed(args.port, args.baud, args.fast,
                                      args.timeout)
        baud = args.fast
        if body.strip():
            sys.stdout.write(body)
        # **「切り替わった」と言い切らない** ([V4])。EOT が来なくても次の
        # コマンドで分かるので、ここでは見えた事実だけを書く。
        print("[fast] %d -> %dbps, EOT %s"
              % (args.baud, args.fast, "ok" if ok else "not seen"))
    else:
        port = open_port(args.port, baud)
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
        print("[rshell_serial] %s %dbps — 'exit' でゲストの rshell も閉じる" % (args.port, baud))
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
