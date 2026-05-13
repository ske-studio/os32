#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
v86_bda_annotate.py - PC-98 BDA アノテーション付きダンプ

v86_bda_*.bin (512B raw) を読み込み、PC-98 BDA フィールド名・説明付きの
可読テキストを出力する。v86_bda.h の定数定義と同期。

使用方法:
    python3 v86_bda_annotate.py v86_bda_init.bin
    python3 v86_bda_annotate.py v86_bda_init.bin --format markdown
    python3 v86_bda_annotate.py v86_bda_init.bin --nonzero
    python3 v86_bda_annotate.py v86_bda_init.bin v86_bda_exit.bin --diff
"""

import struct
import sys
import argparse

BDA_BASE = 0x0400
BDA_SIZE = 512

# PC-98 BDA フィールド辞書 (v86_bda.h + PC9800Bible 準拠)
# (offset_from_0x400, size, name, description)
BDA_FIELDS = [
    # システム情報
    (0x000, 1, "BIOS_FLAG2",     "機種フラグ"),
    (0x001, 1, "EXPMMSZ",        "拡張メモリサイズ (未使用)"),
    (0x013, 2, "MEM_SIZE",       "コンベンショナルメモリサイズ (KB)"),
    (0x058, 1, "BIOS_FLAG5",     "NESA/WAIT フラグ"),
    (0x080, 1, "CPU_FLAG",       "CPU種別フラグ"),
    (0x082, 1, "SCSI_HD",        "SCSI HD接続状態"),
    (0x084, 1, "CPU_TYPE",       "CPUタイプ (03=i386以上)"),
    (0x095, 1, "GRCG",           "GRAPH_CHG: GRCG状態"),
    (0x096, 1, "TILE_REG0",      "タイルレジスタ[0]"),
    (0x097, 1, "TILE_REG1",      "タイルレジスタ[1]"),
    (0x098, 1, "TILE_REG2",      "タイルレジスタ[2]"),
    (0x099, 1, "TILE_REG3",      "タイルレジスタ[3]"),

    # BIOS フラグ・CRT
    (0x101, 1, "BIOS_FLAG",      "BIOS_FLAG (クロック/CPU/メモリ)"),
    (0x13C, 1, "CRT_STS",        "CRT状態フラグ"),

    # キーボードバッファ
    (0x102, 2, "KB_BUF_START",   "キーバッファ先頭アドレス"),
    (0x122, 2, "KB_BUF_END",     "キーバッファ末尾アドレス"),
    (0x124, 2, "KB_HEAD",        "キーバッファ取出ポインタ (WORD)"),
    (0x126, 2, "KB_TAIL",        "キーバッファ入力ポインタ (WORD)"),
    (0x128, 1, "KB_COUNT",       "キーバッファ内キー数"),
    (0x12A, 16, "KB_KEY_STS",    "キー押下状態テーブル (128ビット)"),

    # ディスク
    (0x15C, 2, "DISK_EQUIP",     "ディスク接続状態 (WORD)"),
    (0x15D, 1, "SASI_IDE",       "SASI/IDE HDD接続情報"),
    (0x164, 8, "FDC_RESULT",     "FDC結果バッファ (8バイト)"),
    (0x184, 1, "BOOT_DEV",       "ブートデバイス (DA/UA)"),

    # メモリ
    (0x1AE, 1, "CONV_MEM",       "コンベンショナルメモリ (x4KB)"),
]

# offset → (name, size, desc) のルックアップテーブル
_FIELD_MAP = {}
for _off, _sz, _name, _desc in BDA_FIELDS:
    for _i in range(_sz):
        if _off + _i not in _FIELD_MAP:
            _FIELD_MAP[_off + _i] = (_name, _sz, _desc, _i)


def load_bda(path):
    """BDA raw バイナリを読み込む"""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < BDA_SIZE:
        print(f"WARNING: {path} is {len(data)} bytes (expected {BDA_SIZE})",
              file=sys.stderr)
        data += b"\x00" * (BDA_SIZE - len(data))
    return data[:BDA_SIZE]


def get_field_info(offset):
    """BDA offset (0-511) に対応するフィールド情報を返す"""
    if offset in _FIELD_MAP:
        name, size, desc, sub_off = _FIELD_MAP[offset]
        if sub_off > 0:
            return f"{name}+{sub_off}", size, desc
        return name, size, desc
    return None, 0, ""


def annotate_text(data, nonzero_only=False):
    """テキスト形式のアノテーション出力"""
    lines = []
    lines.append(f"PC-98 BDA Annotated Dump ({len(data)} bytes)")
    lines.append(f"{'Addr':>6s}  {'Hex':>4s}  {'Dec':>5s}  Field")
    lines.append(f"{'----':>6s}  {'---':>4s}  {'---':>5s}  -----")

    # フィールド単位で出力 (登録済みフィールドを優先)
    shown = set()
    for off, sz, name, desc in BDA_FIELDS:
        if off >= BDA_SIZE:
            continue
        end = min(off + sz, BDA_SIZE)
        chunk = data[off:end]

        if nonzero_only and all(b == 0 for b in chunk):
            continue

        addr = BDA_BASE + off
        if sz == 1:
            val = chunk[0]
            hex_str = f"{val:02X}"
            dec_str = f"{val:3d}"
        elif sz == 2:
            val = chunk[0] | (chunk[1] << 8) if len(chunk) >= 2 else chunk[0]
            hex_str = f"{val:04X}"
            dec_str = f"{val:5d}"
        else:
            hex_str = " ".join(f"{b:02X}" for b in chunk)
            dec_str = f"({sz}B)"

        lines.append(f"  {addr:04X}h  {hex_str:>4s}  {dec_str:>5s}  "
                      f"{name} ; {desc}")
        for i in range(off, end):
            shown.add(i)

    # 未登録で非ゼロのバイト
    unregistered = []
    for i in range(BDA_SIZE):
        if i not in shown and data[i] != 0:
            addr = BDA_BASE + i
            unregistered.append(
                f"  {addr:04X}h  {data[i]:02X}    {data[i]:3d}  "
                f"(unknown)")
    if unregistered:
        lines.append("")
        lines.append("--- Unregistered non-zero bytes ---")
        lines.extend(unregistered)

    return "\n".join(lines)


def annotate_markdown(data, nonzero_only=False):
    """Markdown 表形式のアノテーション出力"""
    lines = []
    lines.append("# PC-98 BDA Annotated Dump")
    lines.append("")
    lines.append("| Address | Hex | Dec | Field | Description |")
    lines.append("|---------|-----|-----|-------|-------------|")

    for off, sz, name, desc in BDA_FIELDS:
        if off >= BDA_SIZE:
            continue
        end = min(off + sz, BDA_SIZE)
        chunk = data[off:end]

        if nonzero_only and all(b == 0 for b in chunk):
            continue

        addr = BDA_BASE + off
        if sz == 1:
            val = chunk[0]
            hex_str = f"`{val:02X}`"
            dec_str = f"{val}"
        elif sz == 2:
            val = chunk[0] | (chunk[1] << 8) if len(chunk) >= 2 else chunk[0]
            hex_str = f"`{val:04X}`"
            dec_str = f"{val}"
        else:
            hex_str = "`" + " ".join(f"{b:02X}" for b in chunk[:8])
            if sz > 8:
                hex_str += " ..."
            hex_str += "`"
            dec_str = f"({sz}B)"

        lines.append(
            f"| `{addr:04X}h` | {hex_str} | {dec_str} | "
            f"{name} | {desc} |")

    return "\n".join(lines)


def diff_annotated(data_a, name_a, data_b, name_b):
    """2つのBDAバイナリのフィールド単位差分"""
    lines = []
    lines.append(f"BDA Field Diff: {name_a} -> {name_b}")
    lines.append(f"{'Addr':>6s}  {'Field':<16s}  "
                 f"{name_a:>8s}  {name_b:>8s}  Status")
    lines.append(f"{'----':>6s}  {'-----':<16s}  "
                 f"{'--------':>8s}  {'--------':>8s}  ------")

    diff_count = 0
    for off, sz, name, desc in BDA_FIELDS:
        if off >= BDA_SIZE:
            continue
        end = min(off + sz, BDA_SIZE)
        chunk_a = data_a[off:end]
        chunk_b = data_b[off:end]

        if chunk_a == chunk_b:
            continue

        addr = BDA_BASE + off
        if sz <= 2:
            if sz == 1:
                va = chunk_a[0]
                vb = chunk_b[0]
                ha = f"{va:02X}"
                hb = f"{vb:02X}"
            else:
                va = chunk_a[0] | (chunk_a[1] << 8)
                vb = chunk_b[0] | (chunk_b[1] << 8)
                ha = f"{va:04X}"
                hb = f"{vb:04X}"

            status = "CHANGED"
            if va == 0 and vb != 0:
                status = "uninit?"
            elif va != 0 and vb == 0:
                status = "cleared"
        else:
            ha = " ".join(f"{b:02X}" for b in chunk_a[:4]) + "..."
            hb = " ".join(f"{b:02X}" for b in chunk_b[:4]) + "..."
            status = "CHANGED"

        lines.append(f"  {addr:04X}h  {name:<16s}  "
                     f"{ha:>8s}  {hb:>8s}  {status}")
        diff_count += 1

    # 未登録バイトの差分
    unreg_diffs = 0
    for i in range(BDA_SIZE):
        if data_a[i] != data_b[i]:
            info = get_field_info(i)
            if info[0] is None:
                unreg_diffs += 1

    lines.append("")
    lines.append(f"--- {diff_count} registered fields changed, "
                 f"{unreg_diffs} unregistered bytes changed ---")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="PC-98 BDA アノテーション付きダンプ")
    parser.add_argument("files", nargs="+",
                        help="BDA raw バイナリファイル (512B)")
    parser.add_argument("--format", choices=["text", "markdown"],
                        default="text",
                        help="出力フォーマット (default: text)")
    parser.add_argument("--nonzero", action="store_true",
                        help="非ゼロフィールドのみ表示")
    parser.add_argument("--diff", action="store_true",
                        help="2ファイル間のフィールド単位差分を表示")
    args = parser.parse_args()

    if args.diff:
        if len(args.files) < 2:
            parser.error("--diff には2つ以上のファイルが必要です")
        for i in range(len(args.files) - 1):
            data_a = load_bda(args.files[i])
            data_b = load_bda(args.files[i + 1])
            name_a = args.files[i].rsplit("/", 1)[-1].rsplit("\\", 1)[-1]
            name_b = args.files[i + 1].rsplit("/", 1)[-1].rsplit("\\", 1)[-1]
            print(diff_annotated(data_a, name_a, data_b, name_b))
            if i < len(args.files) - 2:
                print()
        return

    for path in args.files:
        data = load_bda(path)
        tag = path.rsplit("/", 1)[-1].rsplit("\\", 1)[-1]
        print(f"=== {tag} ===")
        if args.format == "markdown":
            print(annotate_markdown(data, args.nonzero))
        else:
            print(annotate_text(data, args.nonzero))
        print()


if __name__ == "__main__":
    main()
