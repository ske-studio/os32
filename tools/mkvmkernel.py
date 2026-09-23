#!/usr/bin/env python3
# ========================================================================
#  mkvmkernel.py -- VK32圧縮カーネルイメージ生成ツール
#
#  kernel.bin と sqlite.bin を LZ4ブロック圧縮し、VK32ヘッダ付き
#  イメージファイル (vmkernel.lz4) を生成する。
#
#  使用法:
#    python3 tools/mkvmkernel.py \
#        --kernel kernel.bin --kernel-addr 0x100000 \
#        --sqlite sqlite.bin --sqlite-addr 0x300000 \
#        -o vmkernel.lz4
#
#  必要パッケージ: pip install lz4
#
#  圧縮は LZ4 **高圧縮 (HC) level 12**。出力は同じ LZ4 ブロック形式なので
#  展開側 (boot/lz4_mini.c・boot/loader_fat_new.asm の pm_lz4_decode) は無変更。
#  生成したイメージが HDD ローダの上限 MAX_IMAGE_SIZE (boot/boot_defs.h) を
#  超えたら**出力を書かずに失敗**する (TASK_HDD_INSTALL N8 の生成側)。
#  ホスト試験: tools/tests/test_vmkernel_lz4.py
#
#  VK32ヘッダ仕様:
#    Offset  Size  Field
#    0x00    4     magic: 'VK32' (0x32334B56 LE)
#    0x04    4     header_size: 16 + entry_count * 16
#    0x08    4     version: 1
#    0x0C    4     entry_count: エントリ数
#    --- entry[i] ---
#    +0x00   4     load_addr: 展開先アドレス
#    +0x04   4     raw_size: 展開後サイズ
#    +0x08   4     data_offset: ファイル先頭からの絶対オフセット
#    +0x0C   4     compressed_size: LZ4圧縮後サイズ
# ========================================================================

import os
import re
import sys
import struct
import argparse

try:
    import lz4.block
except ImportError:
    print("Error: lz4 package not found. Install with: pip install lz4",
          file=sys.stderr)
    sys.exit(1)

# VK32 定数
VK32_MAGIC   = 0x32334B56  # 'VK32' LE
VK32_VERSION = 1

# LZ4 高圧縮の level。2026-09-24 の実測 (kernel 293,832 B + sqlite 374,840 B):
#   既定 (fast)  kernel 210,296 + sqlite 306,387 = 516,731 B (上限まで残り 3.4KB)
#   HC level 9   kernel 179,505 + sqlite 258,519 = 438,072 B
#   HC level 12  kernel 179,132 + sqlite 257,989 = 437,169 B
# 12 (LZ4HC の最大、optimal parser) を採る。9 より約 0.9KB 小さく、圧縮時間は
# ホストで 0.03 秒と無視できる。展開の手間はどの level でも同じ (形式が同じ) で、
# 起動時間は読むバイト数が減るぶん速くなる方向にしか動かない。
LZ4_HC_LEVEL = 12

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 上限の正典。ローダはここでファイルを 0x10000 から読み、超えれば止まる。
DEFAULT_LIMIT_HEADER = os.path.join(ROOT, 'boot', 'boot_defs.h')


def read_max_image_size(path):
    """boot_defs.h の `#define MAX_IMAGE_SIZE (508*1024)` を読んで整数で返す。

    読めない・式が想定外なら例外にする (上限を見ずに通すことはしない)。"""
    with open(path, encoding='utf-8') as f:
        text = f.read()
    m = re.search(r'^#define\s+MAX_IMAGE_SIZE\s+(.+?)\s*(?:/\*.*)?$',
                  text, re.MULTILINE)
    if not m:
        raise ValueError('MAX_IMAGE_SIZE が見つからない: {}'.format(path))
    expr = m.group(1).strip()
    # 数字・空白・括弧・* + だけを許す (任意の式は評価しない)
    if not re.fullmatch(r'[0-9xXa-fA-FuUlL\s()*+]+', expr):
        raise ValueError('MAX_IMAGE_SIZE の式を解釈できない: {!r}'.format(expr))
    expr = re.sub(r'(?<=[0-9a-fA-F])[uUlL]+', '', expr)
    value = eval(expr, {'__builtins__': {}}, {})
    if not isinstance(value, int) or value <= 0:
        raise ValueError('MAX_IMAGE_SIZE が正の整数でない: {!r}'.format(expr))
    return value


def parse_addr(s):
    """16進数アドレス文字列をパース"""
    return int(s, 0)


def main():
    parser = argparse.ArgumentParser(
        description='VK32圧縮カーネルイメージ (vmkernel.lz4) 生成ツール')
    parser.add_argument('--kernel', required=True,
                        help='カーネルバイナリ (kernel.bin)')
    parser.add_argument('--kernel-addr', required=True, type=parse_addr,
                        help='カーネル展開先アドレス (例: 0x100000)')
    parser.add_argument('--sqlite', required=True,
                        help='SQLiteバイナリ (sqlite.bin)')
    parser.add_argument('--sqlite-addr', required=True, type=parse_addr,
                        help='SQLite展開先アドレス (例: 0x300000)')
    parser.add_argument('-o', '--output', required=True,
                        help='出力ファイル名 (vmkernel.lz4)')
    parser.add_argument('--limit-header', default=DEFAULT_LIMIT_HEADER,
                        help='MAX_IMAGE_SIZE を読むヘッダ (既定: boot/boot_defs.h)')
    args = parser.parse_args()

    try:
        max_image_size = read_max_image_size(args.limit_header)
    except (OSError, ValueError) as e:
        print('Error: {}'.format(e), file=sys.stderr)
        sys.exit(1)

    # エントリ定義: (ファイルパス, 展開先アドレス, ラベル)
    entries = [
        (args.kernel, args.kernel_addr, 'kernel'),
        (args.sqlite, args.sqlite_addr, 'sqlite'),
    ]

    entry_count = len(entries)
    common_hdr_size = 16  # magic + header_size + version + entry_count
    header_size = common_hdr_size + entry_count * 16

    # 各エントリの圧縮データを準備
    compressed_list = []
    raw_sizes = []
    for path, addr, label in entries:
        with open(path, 'rb') as f:
            raw_data = f.read()
        raw_size = len(raw_data)
        compressed = lz4.block.compress(raw_data, store_size=False,
                                        mode='high_compression',
                                        compression=LZ4_HC_LEVEL)
        compressed_list.append(compressed)
        raw_sizes.append(raw_size)

    # data_offset を計算 (ファイル先頭からの絶対オフセット)
    data_offsets = []
    offset = header_size
    for comp in compressed_list:
        data_offsets.append(offset)
        offset += len(comp)

    # VK32ヘッダ構築
    hdr = struct.pack('<4I', VK32_MAGIC, header_size, VK32_VERSION, entry_count)

    for i in range(entry_count):
        path, addr, label = entries[i]
        entry = struct.pack('<4I',
                            addr,
                            raw_sizes[i],
                            data_offsets[i],
                            len(compressed_list[i]))
        hdr += entry

    # 上限の検査。超えたイメージはローダが読まずに止まる (boot_main.c の
    # "vmkernel.lz4 too large")。起動しないものを配備させないよう、ここで落とす。
    # 古い出力が残っていると make が最新と見なして配備されうるので消す。
    if offset > max_image_size:
        print('Error: {} は {} バイトで MAX_IMAGE_SIZE ({} バイト, {}) を '
              '{} バイト超える'.format(args.output, offset, max_image_size,
                                     args.limit_header, offset - max_image_size),
              file=sys.stderr)
        try:
            os.remove(args.output)
        except OSError:
            pass
        sys.exit(1)

    # 出力ファイル書き込み
    with open(args.output, 'wb') as f:
        f.write(hdr)
        for comp in compressed_list:
            f.write(comp)

    # 結果表示
    total_size = offset
    print("=== VK32 Image: {} ===".format(args.output))
    print("  magic=0x{:08X} version={} entries={} lz4=HC level {}".format(
        VK32_MAGIC, VK32_VERSION, entry_count, LZ4_HC_LEVEL))
    for i in range(entry_count):
        path, addr, label = entries[i]
        comp_size = len(compressed_list[i])
        ratio = 100 * comp_size // raw_sizes[i] if raw_sizes[i] > 0 else 0
        print("  [{}] {}: addr=0x{:X} raw={} compressed={} ({}%)".format(
            i, label, addr, raw_sizes[i], comp_size, ratio))
    print("  total: {} bytes ({} KB), limit {} bytes (残り {})".format(
        total_size, total_size // 1024, max_image_size,
        max_image_size - total_size))


if __name__ == '__main__':
    main()
