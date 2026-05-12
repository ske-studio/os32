#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
v86_dos_bda_parse.py - T4.1 NP21/W リファレンス BDA パーサ

DEBUG.COM の D コマンド出力 (16進ダンプ ASCII) を入力として、
512バイト raw バイナリに変換する。
RS-232C 経由の raw バイナリ入力にも対応。

使用例:
  # 案1': DEBUG.COM のファイルリダイレクト出力をパース
  python3 v86_dos_bda_parse.py captured.txt -o reference_dos.bin

  # 案2': RS-232C raw バイナリをそのまま変換
  python3 v86_dos_bda_parse.py --raw serial_capture.bin -o reference_dos.bin

  # 注釈付きMarkdown出力
  python3 v86_dos_bda_parse.py captured.txt --annotate > bda_annotated.md

出典: docs/tasks/v86/13_debug_tools_design.md T4.1
"""

import sys
import os
import re
import struct
import argparse

# PC-98 BDA フィールド名辞書 (offset from 0x400)
# kernel/v86_bda.h の #define BDA_* から抽出
BDA_FIELDS = {
    0x000: "BIOS_FLAG2",
    0x001: "EXPMMSZ",
    0x013: "MEM_SIZE (WORD, KB)",
    0x058: "BIOS_FLAG5 (NESA/WAIT)",
    0x080: "CPU_FLAG",
    0x082: "SCSI_HD",
    0x084: "CPU_TYPE (03=i386+)",
    0x095: "GRCG",
    0x096: "TILE_REG[0]",
    0x097: "TILE_REG[1]",
    0x098: "TILE_REG[2]",
    0x099: "TILE_REG[3]",
    0x101: "BIOS_FLAG",
    0x102: "KB_BUF_START",
    0x122: "KB_BUF_END",
    0x124: "KB_HEAD",
    0x126: "KB_TAIL",
    0x128: "KB_COUNT",
    0x12A: "KB_KEY_STS (16B)",
    0x13C: "CRT_STS_FLAG",
    0x15C: "DISK_EQUIP (WORD)",
    0x15D: "SASI_IDE",
    0x164: "FDC_RESULT (8B)",
    0x184: "BOOT_DEV (DA/UA)",
    0x1AE: "CONV_MEM (x4KB)",
}


def parse_debug_dump(text):
    """
    DEBUG.COM の D コマンド出力をパースして bytes に変換する。

    形式例:
    0000:0400  00 00 00 00 00 00 00 00-00 00 00 00 00 00 00 00   ................
    0000:0410  00 00 00 80 02 00 00 00-00 00 00 00 00 00 00 00   ................
    """
    data = bytearray(512)
    base_addr = 0x400  # BDA開始アドレス

    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue

        # アドレス:オフセット ヘックス列 形式を検出
        # "0000:0400  XX XX XX ..." or "0040:0000  XX XX XX ..."
        m = re.match(
            r'([0-9A-Fa-f]{4}):([0-9A-Fa-f]{4})\s+'
            r'((?:[0-9A-Fa-f]{2}[\s\-])+)',
            line
        )
        if not m:
            continue

        seg = int(m.group(1), 16)
        off = int(m.group(2), 16)
        linear = seg * 16 + off  # セグメント:オフセット → リニアアドレス

        # 16進数バイト列を抽出 (ハイフン区切り対応)
        hex_part = m.group(3)
        hex_bytes = re.findall(r'[0-9A-Fa-f]{2}', hex_part)

        for i, hb in enumerate(hex_bytes):
            addr = linear + i - base_addr
            if 0 <= addr < 512:
                data[addr] = int(hb, 16)

    return bytes(data)


def parse_raw_binary(filepath):
    """
    RS-232C 経由の raw バイナリを読み込む。
    終端マーカー (0xFF 0xFF 0xFF 0xFF) があれば切り出す。
    """
    with open(filepath, 'rb') as f:
        raw = f.read()

    # 終端マーカーを検索
    marker = b'\xFF\xFF\xFF\xFF'
    idx = raw.find(marker)
    if idx >= 0:
        raw = raw[:idx]

    if len(raw) < 512:
        # 不足分を0で埋める
        raw = raw + b'\x00' * (512 - len(raw))
    elif len(raw) > 512:
        raw = raw[:512]

    return raw


def annotate_bda(data):
    """
    BDA の注釈付き Markdown を生成する。
    """
    lines = []
    lines.append("# BDA Annotated Dump")
    lines.append("")
    lines.append("| Offset | Hex | Field |")
    lines.append("|--------|-----|-------|")

    for i in range(0, 512, 1):
        field = BDA_FIELDS.get(i, "")
        val = data[i]
        if field or val != 0:
            lines.append(
                f"| 0x{0x400+i:04X} | {val:02X} | {field} |"
            )

    return "\n".join(lines)


def generate_c_array(data, varname="v86_dos_ref_bda"):
    """
    C言語の const u8 配列を生成する。
    T2.5 のデータ入力として使用。
    """
    lines = []
    lines.append(f"/* 自動生成: v86_dos_bda_parse.py */")
    lines.append(f"const unsigned char {varname}[512] = {{")

    for i in range(0, 512, 16):
        chunk = data[i:i+16]
        hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
        comment = f"/* 0x{0x400+i:04X} */"
        lines.append(f"    {hex_vals},  {comment}")

    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="PC-98 BDA パーサ (DEBUG.COM出力 / RS-232C raw)"
    )
    parser.add_argument("input", help="入力ファイル (DEBUG.COM出力テキスト or rawバイナリ)")
    parser.add_argument("-o", "--output", help="出力ファイル (512B rawバイナリ)")
    parser.add_argument("--raw", action="store_true",
                        help="入力を raw バイナリとして処理")
    parser.add_argument("--annotate", action="store_true",
                        help="注釈付きMarkdownを stdout に出力")
    parser.add_argument("--c-array", action="store_true",
                        help="C言語 const 配列を stdout に出力")
    args = parser.parse_args()

    # 入力パース
    if args.raw:
        data = parse_raw_binary(args.input)
    else:
        with open(args.input, 'r', encoding='ascii', errors='replace') as f:
            text = f.read()
        data = parse_debug_dump(text)

    # 出力
    if args.output:
        with open(args.output, 'wb') as f:
            f.write(data)
        print(f"[OK] {args.output} ({len(data)} bytes)", file=sys.stderr)

    if args.annotate:
        print(annotate_bda(data))

    if args.c_array:
        print(generate_c_array(data))

    if not args.output and not args.annotate and not args.c_array:
        # デフォルト: stdout にバイナリ出力
        sys.stdout.buffer.write(data)


if __name__ == "__main__":
    main()
