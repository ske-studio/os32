#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
v86_event_decode.py - V86 イベントログデコーダ (T1.1 / T4)

v86_events.log (16B 固定長バイナリ) をテキスト形式に変換する。

使用方法:
    python3 v86_event_decode.py v86_events.log
    python3 v86_event_decode.py v86_events.log --filter INT
    python3 v86_event_decode.py v86_events.log --filter INT --intno 0x1B
"""

import struct
import sys
import argparse

EVENT_SIZE = 16
EVENT_FMT = "<IHHBBBBHh"  # tick(4) cs(2) ip(2) kind(1) arg1(1) arg2(1) arg3(1) cx(2) reserved(2)

EVENT_KINDS = {
    0: "GP",
    1: "INT",
    2: "IRQ_INJ",
    3: "ROM_CALL",
    4: "EXIT",
    5: "TIMEOUT",
    6: "UNKNOWN_OP",
    0xFF: "LOST_MARKER",
}

EXIT_REASONS = {
    0: "none",
    1: "trap_port",
    2: "dos_term",
    3: "reboot",
    4: "bios_rom",
    5: "timeout",
    6: "unknown_op",
    7: "hotkey",
}


def decode_event(data, offset):
    """16B レコードをデコードして辞書で返す"""
    raw = struct.unpack_from(EVENT_FMT, data, offset)
    tick, cs, ip, kind, arg1, arg2, arg3, cx, reserved = raw
    return {
        "tick": tick,
        "cs": cs,
        "ip": ip,
        "kind": kind,
        "kind_name": EVENT_KINDS.get(kind, f"UNK({kind})"),
        "arg1": arg1,
        "arg2": arg2,
        "arg3": arg3,
        "cx": cx,
    }


def format_event(ev, idx):
    """イベントを1行テキストに整形する"""
    kind = ev["kind_name"]
    tick = ev["tick"]
    cs = ev["cs"]
    ip = ev["ip"]

    if ev["kind"] == 0xFF:
        # LOST マーカー
        return f"  {idx:5d}  *** {tick} events LOST (buffer overflow) ***"

    base = f"  {idx:5d}  {tick:08X}  {cs:04X}:{ip:04X}  {kind:<10s}"

    if ev["kind"] == 1:  # INT
        return f"{base}  INT {ev['arg1']:02X}h  AX={ev['arg2']:02X}{ev['arg3']:02X}  CX={ev['cx']:04X}"
    elif ev["kind"] == 0:  # GP
        return f"{base}  OP={ev['arg1']:02X}h  AX={ev['arg2']:02X}{ev['arg3']:02X}  CX={ev['cx']:04X}"
    elif ev["kind"] == 2:  # IRQ_INJ
        return f"{base}  IRQ={ev['arg1']}  VEC={ev['arg2']:02X}h"
    elif ev["kind"] in (4, 5, 6):  # EXIT/TIMEOUT/UNKNOWN_OP
        reason = EXIT_REASONS.get(ev["arg1"], f"code={ev['arg1']}")
        return f"{base}  reason={reason}"
    else:
        return f"{base}  arg1={ev['arg1']:02X} arg2={ev['arg2']:02X} arg3={ev['arg3']:02X}"


def main():
    parser = argparse.ArgumentParser(description="V86 イベントログデコーダ")
    parser.add_argument("logfile", help="v86_events.log ファイルパス")
    parser.add_argument("--filter", choices=["GP", "INT", "IRQ_INJ", "EXIT", "TIMEOUT"],
                        help="指定した種別のみ表示")
    parser.add_argument("--intno", type=lambda x: int(x, 0),
                        help="INT フィルタ時の割り込み番号 (例: 0x1B)")
    parser.add_argument("--tail", type=int, default=0,
                        help="最後の N 件のみ表示")
    args = parser.parse_args()

    with open(args.logfile, "rb") as f:
        data = f.read()

    total = len(data) // EVENT_SIZE
    if total == 0:
        print("(empty log)")
        return

    # デコード
    events = []
    for i in range(total):
        ev = decode_event(data, i * EVENT_SIZE)
        events.append(ev)

    # フィルタ
    if args.filter:
        kind_map = {v: k for k, v in EVENT_KINDS.items()}
        target_kind = kind_map.get(args.filter, -1)
        filtered = []
        for ev in events:
            if ev["kind"] == target_kind:
                if args.intno is not None and ev["kind"] == 1:
                    if ev["arg1"] != args.intno:
                        continue
                filtered.append(ev)
        events = filtered

    # tail
    if args.tail > 0:
        events = events[-args.tail:]

    # 出力
    print(f"V86 Event Log: {total} records ({len(data)} bytes)")
    print(f"  #     Tick      CS:IP       Kind        Details")
    print(f"  ----  --------  ----------  ----------  -------")

    for i, ev in enumerate(events):
        print(format_event(ev, i + 1))

    # 統計サマリ
    print()
    print(f"--- Summary ---")
    counts = {}
    for ev in events:
        name = ev["kind_name"]
        counts[name] = counts.get(name, 0) + 1
    for name, cnt in sorted(counts.items(), key=lambda x: -x[1]):
        print(f"  {name:<12s}: {cnt}")


if __name__ == "__main__":
    main()
