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
    args = parser.parse_args()

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
        compressed = lz4.block.compress(raw_data, store_size=False)
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

    # 出力ファイル書き込み
    with open(args.output, 'wb') as f:
        f.write(hdr)
        for comp in compressed_list:
            f.write(comp)

    # 結果表示
    total_size = offset
    print("=== VK32 Image: {} ===".format(args.output))
    print("  magic=0x{:08X} version={} entries={}".format(
        VK32_MAGIC, VK32_VERSION, entry_count))
    for i in range(entry_count):
        path, addr, label = entries[i]
        comp_size = len(compressed_list[i])
        ratio = 100 * comp_size // raw_sizes[i] if raw_sizes[i] > 0 else 0
        print("  [{}] {}: addr=0x{:X} raw={} compressed={} ({}%)".format(
            i, label, addr, raw_sizes[i], comp_size, ratio))
    print("  total: {} bytes ({} KB)".format(total_size, total_size // 1024))


if __name__ == '__main__':
    main()
