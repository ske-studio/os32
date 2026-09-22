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
  3. ポートを閉じて N で開き直し、**`ver` を投げて EOT まで返るか見る** (5 秒)
  4. 返れば `linked at N`、以後のコマンドは N で送る
  5. 返らなければ **元の速度へ戻って `ver` で確かめ**、
     `fast switch failed, back at 9600` と報告して終了コード 1
ゲスト側にも番犬があり、**切替後 5 秒 (500 tick) 無音なら自力で元へ戻す**
(userland/shell/serial_watchdog.c)。だから「FIFO 無し」「013Ah が効かない」
「ケーブルが速度に耐えない」のどれでも会話は 9600 で生き残る。
N が --baud と同じなら切り替えは行わない。
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
# 切替の確認に使う実コマンド。**副作用が無く、応答が短く、必ず EOT で閉じる**もの。
PROBE_CMD = "ver"
# **EOT だけで判定しない** (Codex レビュー B3)。`serial N` の終了処理が
# SPEED_SWITCH_SETTLE_S を超えると、`ver` を送ったあとに **切替コマンドの EOT**
# が届き、それを `ver` の成功と読んでしまう → 以後の応答が 1 コマンドずれる。
# `ver` の本文にしか出ない文字列まで見る (userland/shell/cmd_base.c の
#   kprintf("  Build: %s %s\n", __DATE__, __TIME__) )。
PROBE_EXPECT = "Build:"
# `serial N` を送ったあと、旧速度で EOT を 1 つ待つ上限 [秒]。旧速度で返る
# EOT は切替の途中で化けるので**来ない前提**だが、来たならそこまでで切り替えて
# よい。待つこと自体に意味がある (ゲストが 013Ah を書き終える時間)。
SWITCH_DRAIN_S = 2.0
# 確認の往復に許す秒数。[V3] の 15 秒は「長いコマンド」の話で、ここは
# 「速度が合っているか」の判定 — 合っていれば 1 秒で返り、合っていなければ
# 何秒待っても返らない。5 秒はゲスト側の番犬 (500 tick) と同じ尺度。
SWITCH_PROBE_TIMEOUT_S = 5.0
# 失敗したあと、ゲストの番犬が元の速度へ戻すのを待つ秒数 (番犬は 5 秒 + 余裕)。
WATCHDOG_WAIT_S = 6.0


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


def probe(port, timeout_s):
    """実コマンド (`ver`) を投げて、**本文まで**返るか見る。

    **受動待ちでは判定にならない。** 切替の行の EOT は化けて消えることがあり、
    「来ないこと」は失敗の証拠にも成功の証拠にもならない。こちらから 1 行
    送って往復が成立するかを見れば、速度が合っているかがそのまま分かる
    (往復 1 blocker 3: 成功時に 15 秒待たされるのもこれで消える)。

    **EOT だけを見てはいけない** (往復 2 B3)。先行コマンドの EOT が遅れて
    届くと、それを `ver` の成功と読んで以後の応答が 1 コマンドずれる。
    `ver` の本文にしか出ない `Build:` まで確かめる。
    """
    try:
        text, ok = send_cmd(port, PROBE_CMD, timeout_s)
    except Exception:            # pragma: no cover — 速度不一致で化けたとき
        return "", False
    if not ok:
        return text, False
    if PROBE_EXPECT not in text:
        # EOT は来たが `ver` の応答ではない = 先行コマンドの EOT を拾った、
        # または化けている。**成功に数えない。**
        return text, False
    return text, True


def check_echo(text, line):
    """応答が `> <送ったコマンド>` のエコー行で始まっているか。

    ゲストの rshell は実行前に `> <コマンド>` を出す (userland/shell/rshell.c)。
    ここがずれていたら EOT の対応が 1 つずれている (往復 2 B3)。
    """
    head = text.lstrip("\r\n")
    return head.startswith("> " + line)


def switch_speed(port_name, open_baud, fast_baud, timeout_s):
    """ゲストを fast_baud へ切り替える。戻り値 (port, baud, ok, note)。

    **切替の応答は待たない。** ゲストは `serial N` の途中で 013Ah を書き換える
    ので、そこから先のバイトは古い速度側では化ける。書いて掃けるのを待ったら
    閉じ、N で開き直して **`ver` の往復**で足並みを確かめる。

    往復しなければ元の速度へ開き直してもう一度 `ver` を投げる。ゲスト側の
    番犬 (userland/shell/serial_watchdog.c) が 5 秒で元へ戻しているはずなので、
    ここが通れば会話は生き残っている。
    """
    port = open_port(port_name, open_baud)
    try:
        port.reset_input_buffer()
        port.write(("serial %d\n" % fast_baud).encode("ascii"))
        port.flush()
        # 送信 FIFO が掃けて、ゲストが 013Ah を書き終えるまでの間合い。
        # 9600 で 1 文字 ≒ 1.04ms、行は 14 文字。余裕を見て 0.5 秒。
        time.sleep(SPEED_SWITCH_SETTLE_S)
        # **切替コマンドの EOT をここで吸っておく** (往復 2 B3)。
        # 残したまま新速度へ移ると、次に投げる `ver` の EOT と取り違える。
        # 旧速度で返る EOT は切替の途中で化けるので来ない方が普通だが、
        # 来たならそこまでで切り上げる。来なくても 2 秒で進む
        # (この待ち自体がゲストの終了処理を追い越さないための間合い)。
        read_until_eot(port, SWITCH_DRAIN_S)
    finally:
        port.close()

    port = open_port(port_name, fast_baud)
    port.reset_input_buffer()
    _, ok = probe(port, SWITCH_PROBE_TIMEOUT_S)
    if ok:
        return port, fast_baud, True, "linked at %d" % fast_baud

    # 失敗。ゲストの番犬が元へ戻すのを待ってから、元の速度で確かめる。
    port.close()
    time.sleep(WATCHDOG_WAIT_S)
    port = open_port(port_name, open_baud)
    port.reset_input_buffer()
    _, back = probe(port, SWITCH_PROBE_TIMEOUT_S)
    note = ("fast switch failed, back at %d" % open_baud if back
            else "fast switch failed AND %d does not answer either"
                 % open_baud)
    return port, open_baud, False, note


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
        port, baud, ok, note = switch_speed(args.port, args.baud, args.fast,
                                            args.timeout)
        # **「切り替わった」と言い切らない** ([V4]) — `ver` が往復したかだけを
        # 書く。失敗なら元の速度に戻っているので、そのまま終わる。
        print("[rshell_serial] %s" % note)
        if not ok:
            port.close()
            return 1
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
            line = " ".join(args.line)
            text, ok = send_cmd(port, line, args.timeout)
            sys.stdout.write(text)
            if not ok:
                print("\n[rshell_serial] timeout waiting for EOT (%.0fs)" % args.timeout)
                return 1
            # **応答がこのコマンドのものか確かめる** (往復 2 B3)。EOT の対応が
            # 1 つずれていると、以後ずっと前のコマンドの応答を読み続ける。
            if not check_echo(text, line):
                print("[rshell_serial] desync: expected echo of %r" % line)
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
            elif not check_echo(text, line):
                # EOT の対応が 1 つずれている (往復 2 B3)。対話は続けられるが
                # **黙って進まない** — 読んでいる応答が別のコマンドのもの。
                print("[rshell_serial] desync: expected echo of %r" % line)
            if line.strip() == "exit":
                return 0
    finally:
        port.close()


if __name__ == "__main__":
    sys.exit(main())
