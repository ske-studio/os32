#!/usr/bin/env python3
"""
write_ipl.py — test.nhd の IPL セクタにバイナリを書き込むツール

使い方:
  python3 write_ipl.py <binfile> [--sector N] [--count N]

デフォルト: セクタ0 (IPL) に1セクタ (512B) 書き込み
--sector N: 開始セクタ (0=IPL)
--count N: 書き込むセクタ数
"""

import sys
import os
import struct
import argparse

NP21W_DIR = os.environ.get('NP21W_DIR', r'/tmp/np21w')
NHD_PATH = os.path.join(NP21W_DIR, 'test.nhd')

def read_nhd_header(f):
    """NHDヘッダを読んでジオメトリ情報を返す"""
    f.seek(0)
    header = f.read(512)
    sig = header[:15]
    if not sig.startswith(b'T98HDDIMAGE'):
        raise ValueError(f"NHDシグネチャが不正: {sig}")
    
    header_size = struct.unpack_from('<I', header, 272)[0]
    cylinders = struct.unpack_from('<I', header, 276)[0]
    heads = struct.unpack_from('<H', header, 280)[0]
    sectors = struct.unpack_from('<H', header, 282)[0]
    sector_size = struct.unpack_from('<H', header, 284)[0]
    
    return {
        'header_size': header_size,
        'cylinders': cylinders,
        'heads': heads,
        'sectors': sectors,
        'sector_size': sector_size
    }

def main():
    parser = argparse.ArgumentParser(description='NHD IPL書き込みツール')
    parser.add_argument('binfile', help='書き込むバイナリファイル')
    parser.add_argument('--sector', type=int, default=0, help='開始セクタ番号 (デフォルト: 0)')
    parser.add_argument('--count', type=int, default=0, help='書き込むセクタ数 (0=自動)')
    parser.add_argument('--nhd', default=NHD_PATH, help='NHDファイルパス')
    args = parser.parse_args()
    
    # バイナリ読み込み
    with open(args.binfile, 'rb') as f:
        bin_data = f.read()
    print(f"バイナリ: {args.binfile} ({len(bin_data)} bytes)")
    
    # NHDヘッダ読み込み
    with open(args.nhd, 'r+b') as f:
        geo = read_nhd_header(f)
        print(f"NHD: {geo['cylinders']}cyl x {geo['heads']}head x {geo['sectors']}spt x {geo['sector_size']}B")
        
        sector_size = geo['sector_size']
        offset = geo['header_size'] + args.sector * sector_size
        
        if args.count == 0:
            # 自動計算: バイナリサイズから必要なセクタ数を決定
            num_sectors = (len(bin_data) + sector_size - 1) // sector_size
        else:
            num_sectors = args.count
        
        # パディング
        write_data = bin_data[:num_sectors * sector_size]
        write_data = write_data.ljust(num_sectors * sector_size, b'\x00')
        
        print(f"書き込み: セクタ{args.sector} から {num_sectors}セクタ ({len(write_data)} bytes)")
        print(f"ファイルオフセット: 0x{offset:X}")
        
        f.seek(offset)
        f.write(write_data)
        
    print("完了!")

if __name__ == '__main__':
    main()
