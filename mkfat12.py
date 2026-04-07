#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mkfat12.py — PC-98 2HD (1024B/sector) FAT12 フロッピーイメージ作成ツール

PC-98 2HD仕様:
  77シリンダ × 2ヘッド × 8セクタ/トラック = 1232セクタ
  1024バイト/セクタ
  総容量: 1,261,568バイト (1232KB)

FAT12レイアウト:
  セクタ 0:      ブートセクタ (BPB + IPLコード用の空間)
  セクタ 1-2:    FAT1 (2セクタ)
  セクタ 3-4:    FAT2 (2セクタ)
  セクタ 5-10:   ルートディレクトリ (192エントリ × 32B = 6144B = 6セクタ)
  セクタ 11〜:   データ領域 (クラスタ2から開始)

使用例:
  python3 mkfat12.py -o os_raw.img -b boot_fat.bin \\
      LOADER.BIN=loader.bin KERNEL.BIN=kernel.bin SHELL.BIN=shell.bin
"""

import sys
import os
import struct
import argparse

# PC-98 2HD FAT12パラメータ
BYTES_PER_SECTOR = 1024
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 1        # ブートセクタ
NUM_FATS = 2
ROOT_ENTRY_COUNT = 192      # ルートディレクトリエントリ数
TOTAL_SECTORS = 1232        # 77cyl × 2head × 8sect
MEDIA_TYPE = 0xFE           # PC-98 2HD
FAT_SIZE = 2                # FATあたりセクタ数
SECTORS_PER_TRACK = 8
NUM_HEADS = 2
HIDDEN_SECTORS = 0

# 計算値
FAT_START = RESERVED_SECTORS                       # セクタ1
ROOT_DIR_START = FAT_START + NUM_FATS * FAT_SIZE   # セクタ5
ROOT_DIR_SECTORS = (ROOT_ENTRY_COUNT * 32 + BYTES_PER_SECTOR - 1) // BYTES_PER_SECTOR  # 6
DATA_START = ROOT_DIR_START + ROOT_DIR_SECTORS     # セクタ11
TOTAL_DATA_CLUSTERS = TOTAL_SECTORS - DATA_START   # 1221

# FAT12特殊値
FAT12_FREE = 0x000
FAT12_EOC  = 0xFFF
FAT12_MEDIA = 0xFF0 | MEDIA_TYPE  # メディアバイト


def make_bpb():
    """BPB (BIOS Parameter Block) を生成 (オフセット0x00〜0x23, 36バイト)"""
    bpb = bytearray(36)
    # ジャンプ命令 (JMP SHORT xx; NOP) — 後でブートコードで上書き
    bpb[0] = 0xEB  # JMP SHORT
    bpb[1] = 0x22  # +34 (BPBの直後にジャンプ)
    bpb[2] = 0x90  # NOP
    # OEM名 (8バイト)
    oem = b'OS32IPL '
    bpb[3:11] = oem
    # BPBフィールド
    struct.pack_into('<H', bpb, 0x0B, BYTES_PER_SECTOR)
    bpb[0x0D] = SECTORS_PER_CLUSTER
    struct.pack_into('<H', bpb, 0x0E, RESERVED_SECTORS)
    bpb[0x10] = NUM_FATS
    struct.pack_into('<H', bpb, 0x11, ROOT_ENTRY_COUNT)
    struct.pack_into('<H', bpb, 0x13, TOTAL_SECTORS)
    bpb[0x15] = MEDIA_TYPE
    struct.pack_into('<H', bpb, 0x16, FAT_SIZE)
    struct.pack_into('<H', bpb, 0x18, SECTORS_PER_TRACK)
    struct.pack_into('<H', bpb, 0x1A, NUM_HEADS)
    struct.pack_into('<I', bpb, 0x1C, HIDDEN_SECTORS)
    struct.pack_into('<I', bpb, 0x20, 0)  # total_sectors_32 (16bitで足りるので0)
    return bytes(bpb)


def make_fat():
    """FAT12テーブルを初期化 (2セクタ = 2048バイト)"""
    fat = bytearray(FAT_SIZE * BYTES_PER_SECTOR)
    # エントリ0: メディアバイト (0xFFE for 0xFE media)
    # エントリ1: EOC (0xFFF)
    # FAT12: 3バイトで2エントリ
    # entry0 = 0xFFE, entry1 = 0xFFF
    # byte0 = low8(entry0) = 0xFE
    # byte1 = high4(entry0) | low4(entry1)<<4 = 0xFF
    # byte2 = high8(entry1) = 0xFF
    fat[0] = MEDIA_TYPE   # 0xFE
    fat[1] = 0xFF
    fat[2] = 0xFF
    return bytes(fat)


def fat12_get(fat_data, cluster):
    """FAT12エントリを読み取り"""
    offset = cluster + cluster // 2
    if offset + 1 >= len(fat_data):
        return FAT12_EOC
    val = fat_data[offset] | (fat_data[offset + 1] << 8)
    if cluster & 1:
        return (val >> 4) & 0x0FFF
    else:
        return val & 0x0FFF


def fat12_set(fat_data, cluster, value):
    """FAT12エントリを書き込み"""
    offset = cluster + cluster // 2
    if offset + 1 >= len(fat_data):
        return
    val = fat_data[offset] | (fat_data[offset + 1] << 8)
    if cluster & 1:
        val = (val & 0x000F) | ((value & 0x0FFF) << 4)
    else:
        val = (val & 0xF000) | (value & 0x0FFF)
    fat_data[offset] = val & 0xFF
    fat_data[offset + 1] = (val >> 8) & 0xFF


def name_to_83(name):
    """ファイル名を8.3形式 (11バイト) に変換"""
    name = name.upper()
    if '.' in name:
        base, ext = name.rsplit('.', 1)
    else:
        base, ext = name, ''
    base = base[:8].ljust(8)
    ext = ext[:3].ljust(3)
    return (base + ext).encode('ascii')


def make_dir_entry(name83, attr, cluster, size):
    """32バイトのディレクトリエントリを生成"""
    entry = bytearray(32)
    entry[0:11] = name83
    entry[0x0B] = attr
    # 時刻・日付は固定値 (2025-01-01 00:00:00)
    struct.pack_into('<H', entry, 0x16, 0x0000)  # time
    struct.pack_into('<H', entry, 0x18, 0x5A21)  # date
    struct.pack_into('<H', entry, 0x1A, cluster & 0xFFFF)
    struct.pack_into('<I', entry, 0x1C, size)
    return bytes(entry)


def create_fat12_image(boot_bin=None, files=None):
    """FAT12フロッピーイメージを生成"""
    # 全体をゼロで初期化
    image = bytearray(TOTAL_SECTORS * BYTES_PER_SECTOR)
    
    # FAT初期化
    fat = bytearray(make_fat())
    
    # ルートディレクトリ
    root_dir = bytearray(ROOT_DIR_SECTORS * BYTES_PER_SECTOR)
    
    # ファイルを配置
    next_cluster = 2  # 最初のデータクラスタ
    dir_index = 0
    
    if files:
        for fat_name, local_path in files:
            if not os.path.exists(local_path):
                print(f"  警告: {local_path} が見つかりません。スキップ。")
                continue
            
            with open(local_path, 'rb') as f:
                data = f.read()
            
            file_size = len(data)
            clusters_needed = max(1, (file_size + BYTES_PER_SECTOR * SECTORS_PER_CLUSTER - 1) //
                                  (BYTES_PER_SECTOR * SECTORS_PER_CLUSTER))
            
            if next_cluster + clusters_needed > TOTAL_DATA_CLUSTERS + 2:
                print(f"  エラー: ディスク容量不足 ({fat_name})")
                sys.exit(1)
            
            # ディレクトリエントリ作成
            name83 = name_to_83(fat_name)
            entry = make_dir_entry(name83, 0x20, next_cluster, file_size)
            root_dir[dir_index * 32:(dir_index + 1) * 32] = entry
            dir_index += 1
            
            # データ書き込み + FATチェーン設定
            first_cluster = next_cluster
            remaining = file_size
            offset = 0
            
            for i in range(clusters_needed):
                cluster = next_cluster
                # データをクラスタに書き込み
                sector_lba = DATA_START + (cluster - 2) * SECTORS_PER_CLUSTER
                chunk = min(remaining, BYTES_PER_SECTOR * SECTORS_PER_CLUSTER)
                img_offset = sector_lba * BYTES_PER_SECTOR
                image[img_offset:img_offset + chunk] = data[offset:offset + chunk]
                offset += chunk
                remaining -= chunk
                
                # FATエントリ設定
                if i < clusters_needed - 1:
                    fat12_set(fat, cluster, cluster + 1)
                else:
                    fat12_set(fat, cluster, FAT12_EOC)
                
                next_cluster += 1
            
            print(f"  {fat_name:12s} -> クラスタ {first_cluster}-{next_cluster-1} "
                  f"({file_size} bytes, {clusters_needed} clusters)")
    
    # ブートセクタ構築
    boot_sector = bytearray(BYTES_PER_SECTOR)
    bpb = make_bpb()
    
    if boot_bin and os.path.exists(boot_bin):
        # ブートコードを読み込み
        with open(boot_bin, 'rb') as f:
            boot_code = f.read()
        if len(boot_code) > BYTES_PER_SECTOR:
            boot_code = boot_code[:BYTES_PER_SECTOR]
        boot_sector[:len(boot_code)] = boot_code
        # BPBを上書き (オフセット 0x03〜0x23)
        # ただしジャンプ命令(0x00-0x02)はブートコード側を使用
        boot_sector[0x03:0x24] = bpb[0x03:0x24]
    else:
        # BPBのみ書き込み (ブートコードなし)
        boot_sector[:len(bpb)] = bpb
        # ブートシグネチャ
        boot_sector[BYTES_PER_SECTOR - 2] = 0x55
        boot_sector[BYTES_PER_SECTOR - 1] = 0xAA
    
    # イメージに書き込み
    # セクタ0: ブートセクタ
    image[0:BYTES_PER_SECTOR] = boot_sector
    
    # FAT1 (セクタ1-2)
    fat_offset = FAT_START * BYTES_PER_SECTOR
    image[fat_offset:fat_offset + len(fat)] = fat
    
    # FAT2 (セクタ3-4) — FAT1のコピー
    fat2_offset = (FAT_START + FAT_SIZE) * BYTES_PER_SECTOR
    image[fat2_offset:fat2_offset + len(fat)] = fat
    
    # ルートディレクトリ (セクタ5-10)
    root_offset = ROOT_DIR_START * BYTES_PER_SECTOR
    image[root_offset:root_offset + len(root_dir)] = root_dir
    
    return bytes(image)


def print_info():
    """FAT12パラメータ情報を表示"""
    print("PC-98 2HD FAT12 パラメータ:")
    print(f"  セクタサイズ:       {BYTES_PER_SECTOR} bytes")
    print(f"  セクタ/クラスタ:    {SECTORS_PER_CLUSTER}")
    print(f"  予約セクタ:         {RESERVED_SECTORS}")
    print(f"  FAT数:              {NUM_FATS}")
    print(f"  FATサイズ:          {FAT_SIZE} sectors/FAT")
    print(f"  ルートDirエントリ:  {ROOT_ENTRY_COUNT}")
    print(f"  ルートDirセクタ:    {ROOT_DIR_SECTORS}")
    print(f"  総セクタ:           {TOTAL_SECTORS}")
    print(f"  データ開始セクタ:   {DATA_START}")
    print(f"  データクラスタ数:   {TOTAL_DATA_CLUSTERS}")
    print(f"  イメージサイズ:     {TOTAL_SECTORS * BYTES_PER_SECTOR} bytes")
    cluster_bytes = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER
    print(f"  利用可能容量:       {TOTAL_DATA_CLUSTERS * cluster_bytes} bytes "
          f"({TOTAL_DATA_CLUSTERS * cluster_bytes // 1024} KB)")


def main():
    parser = argparse.ArgumentParser(description='PC-98 2HD FAT12フロッピーイメージ作成')
    parser.add_argument('-o', '--output', required=True, help='出力イメージファイル')
    parser.add_argument('-b', '--boot', help='ブートセクタバイナリ (省略時はBPBのみ)')
    parser.add_argument('-d', '--d88', help='D88出力ファイル (mkd88.py連携)')
    parser.add_argument('-i', '--info', action='store_true', help='FAT12パラメータ表示')
    parser.add_argument('files', nargs='*',
                       help='追加ファイル (FAT名=ローカルパス 形式)')
    
    args = parser.parse_args()
    
    if args.info:
        print_info()
        return
    
    print("=== PC-98 2HD FAT12イメージ作成 ===")
    print_info()
    print()
    
    # ファイルリスト解析
    file_list = []
    for spec in args.files:
        if '=' in spec:
            fat_name, local_path = spec.split('=', 1)
        else:
            fat_name = os.path.basename(spec).upper()
            local_path = spec
        file_list.append((fat_name, local_path))
    
    print(f"ブートセクタ: {args.boot or '(BPBのみ)'}")
    print(f"ファイル数:   {len(file_list)}")
    print()
    
    # イメージ生成
    image = create_fat12_image(boot_bin=args.boot, files=file_list)
    
    # 書き出し
    with open(args.output, 'wb') as f:
        f.write(image)
    print(f"\n出力: {args.output} ({len(image)} bytes)")
    
    # D88変換
    if args.d88:
        mkd88_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mkd88.py')
        if os.path.exists(mkd88_path):
            import subprocess
            subprocess.run([sys.executable, mkd88_path, args.output, args.d88], check=True)
            print(f"D88:  {args.d88}")
        else:
            print(f"警告: mkd88.py が見つかりません ({mkd88_path})")


if __name__ == '__main__':
    main()
