#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
v86_bda_diff.py - BDA スナップショット差分レポート (T4.3)

v86_debug_dump_bda_named() が出力する 512B raw バイナリファイル同士を
比較して、変化したオフセットとPC-98 BDA フィールド名を表示する。

使用方法:
    python3 v86_bda_diff.py v86_bda_init.bin v86_bda_exit.bin
    python3 v86_bda_diff.py v86_bda_init.bin v86_bda_post_ipl.bin v86_bda_pre_dos.bin v86_bda_exit.bin
    python3 v86_bda_diff.py --ref np21w_bda_ref.bin v86_bda_init.bin
"""

import struct
import sys
import argparse

BDA_SIZE = 512  # 0x0400-0x05FF

# PC-98 BDA フィールド定義 (オフセット → 名前, サイズ)
# 0x0400 ベースのオフセット
BDA_FIELDS = {
    0x00: ("KB_BUF_HEAD", 2, "キーバッファ HEAD ポインタ"),
    0x02: ("KB_BUF_TAIL", 2, "キーバッファ TAIL ポインタ"),
    0x04: ("KB_COUNT", 1, "キーバッファ内文字数"),
    0x05: ("KB_SHIFT_STATE", 1, "シフトキー状態"),
    0x06: ("KB_BUF_START", 2, "キーバッファ開始オフセット"),
    0x08: ("KB_BUF_END", 2, "キーバッファ終了オフセット"),
    0x0A: ("KB_BUF", 32, "キーバッファ (16文字 x 2B)"),
    0x2A: ("CURSOR_ROW", 1, "カーソル行"),
    0x2B: ("CURSOR_COL", 1, "カーソル列"),
    0x2C: ("CURSOR_ATTR", 1, "カーソル属性"),
    0x4C: ("TIMER_LOW", 2, "タイマカウンタ (下位)"),
    0x4E: ("TIMER_HIGH", 2, "タイマカウンタ (上位)"),
    0x54: ("DISK_STATUS", 1, "FDDステータス"),
    0x56: ("DISK_RESULT", 16, "FDD結果テーブル"),
    0x6C: ("EQUIP_FLAG", 2, "機器構成フラグ"),
    0x72: ("MEM_SIZE", 2, "コンベンショナルメモリサイズ (KB)"),
    0x7C: ("BOOT_FLAG", 1, "ブートフラグ"),
    0x81: ("TEXT_COLS", 1, "テキスト桁数"),
    0x82: ("TEXT_ROWS", 1, "テキスト行数"),
}


def find_field(offset):
    """BDA オフセットに対応するフィールド名を返す"""
    for foff, (name, size, desc) in sorted(BDA_FIELDS.items()):
        if foff <= offset < foff + size:
            return f"{name} (+{offset - foff})", desc
    return f"UNK_0x{0x400 + offset:03X}", ""


def load_bda(path):
    """BDA raw バイナリを読み込む"""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < BDA_SIZE:
        print(f"WARNING: {path} is {len(data)} bytes (expected {BDA_SIZE})", file=sys.stderr)
        data += b"\x00" * (BDA_SIZE - len(data))
    return data[:BDA_SIZE]


def diff_bda(name_a, data_a, name_b, data_b):
    """2つの BDA バイナリを比較して差分を表示する"""
    diffs = []
    for i in range(BDA_SIZE):
        if data_a[i] != data_b[i]:
            field, desc = find_field(i)
            diffs.append((i, data_a[i], data_b[i], field, desc))

    if not diffs:
        print(f"  No differences between {name_a} and {name_b}")
        return 0

    print(f"  Differences: {name_a} -> {name_b}  ({len(diffs)} bytes changed)")
    print(f"  {'Offset':>8s}  {'Old':>4s}  {'New':>4s}  Field")
    print(f"  {'------':>8s}  {'---':>4s}  {'---':>4s}  -----")
    for offset, old, new, field, desc in diffs:
        addr = 0x0400 + offset
        comment = f"  ; {desc}" if desc else ""
        print(f"  {addr:04X}h     {old:02X}    {new:02X}    {field}{comment}")

    return len(diffs)


def main():
    parser = argparse.ArgumentParser(description="BDA スナップショット差分レポート")
    parser.add_argument("files", nargs="+", help="BDA raw バイナリファイル (2個以上)")
    parser.add_argument("--ref", help="リファレンス BDA (NP21/W BIOS 採取) と比較")
    args = parser.parse_args()

    if len(args.files) < 2 and not args.ref:
        parser.error("少なくとも2つのファイルを指定するか、--ref を使用してください")

    files = args.files
    snapshots = []
    for path in files:
        data = load_bda(path)
        # ファイル名からタグを推定
        tag = path.rsplit("/", 1)[-1].rsplit("\\", 1)[-1]
        tag = tag.replace("v86_bda_", "").replace(".bin", "")
        snapshots.append((tag, data))

    # リファレンスとの比較
    if args.ref:
        ref_data = load_bda(args.ref)
        ref_tag = args.ref.rsplit("/", 1)[-1].rsplit("\\", 1)[-1]
        print(f"\n=== Reference comparison ===")
        for tag, data in snapshots:
            diff_bda(ref_tag, ref_data, tag, data)
            print()
        return

    # 連続スナップショット間の比較
    print(f"BDA Snapshot Diff Report")
    print(f"  Snapshots: {', '.join(t for t, _ in snapshots)}")
    print()

    total_diffs = 0
    for i in range(len(snapshots) - 1):
        tag_a, data_a = snapshots[i]
        tag_b, data_b = snapshots[i + 1]
        n = diff_bda(tag_a, data_a, tag_b, data_b)
        total_diffs += n
        print()

    print(f"--- Total: {total_diffs} byte changes across {len(snapshots) - 1} transitions ---")


if __name__ == "__main__":
    main()
