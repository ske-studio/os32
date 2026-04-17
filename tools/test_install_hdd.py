#!/usr/bin/env python3
"""
install_hdd.py — NHDファイルに直接OS32をインストール

IPL + パーティションテーブル + ローダー + カーネル + ext2ファイルシステム を
NHDファイルに直接書き込む。インストーラー(install.bin)を介さずにホスト側で
全て完結する。

使い方:
    python3 tools/install_hdd.py

ext2パーティションはシリンダ1 (LBA 136) から開始:
    LBA 0:     IPL (test_p_boot.bin)
    LBA 1:     PC-98パーティションテーブル
    LBA 2-5:   ローダー (test_p_load.bin)
    LBA 6-135: カーネル (kernel.bin)
    LBA 136+:  ext2ファイルシステム
"""

import sys
import os
import struct
import time
import glob

# === 設定 ===
PROJ_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NP21W_DIR = os.environ.get('NP21W_DIR', r'/tmp/np21w')
NHD_PATH = os.path.join(NP21W_DIR, 'test.nhd')

SECTOR_SIZE = 512
EXT2_BLOCK_SIZE = 1024
EXT2_SUPER_MAGIC = 0xEF53
EXT2_INODE_SIZE = 128
EXT2_ROOT_INO = 2

PC98_HEADS = 8
PC98_SPT = 17
PARTITION_LBA = PC98_HEADS * PC98_SPT * 2  # 272 (シリンダ2から開始: kernel 155セクタを収容)

S_IFDIR = 0o40000
S_IFREG = 0o100000

def read_nhd_geometry(f):
    """NHDヘッダからジオメトリ情報を取得"""
    f.seek(0)
    header = f.read(512)
    if not header[:11].startswith(b'T98HDDIMAGE'):
        raise ValueError("NHDシグネチャが不正")
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

def write_sector(f, nhd_header_size, lba, data):
    """指定LBAにデータを書き込み"""
    f.seek(nhd_header_size + lba * SECTOR_SIZE)
    f.write(data)

def write_sectors(f, nhd_header_size, start_lba, data):
    """連続セクタにデータを書き込み (512B境界にパディング)"""
    padded = data + b'\x00' * (SECTOR_SIZE - len(data) % SECTOR_SIZE) if len(data) % SECTOR_SIZE else data
    f.seek(nhd_header_size + start_lba * SECTOR_SIZE)
    f.write(padded)

def make_partition_table(geo):
    """PC-98パーティションテーブル"""
    pt = bytearray(512)
    end_cyl = geo['cylinders'] - 1
    
    pt[0] = 0x80   # bootable
    pt[1] = 0xE2   # system type: ext2
    pt[6] = 0      # start sector (0開始)
    pt[7] = 0      # start head
    pt[8] = 2      # start cylinder low (シリンダ2)
    pt[9] = 0      # start cylinder high
    pt[10] = PC98_SPT - 1   # end sector
    pt[11] = PC98_HEADS - 1 # end head
    pt[12] = end_cyl & 0xFF        # end cylinder low
    pt[13] = (end_cyl >> 8) & 0xFF # end cylinder high
    
    name = b'OS32            '
    pt[16:32] = name[:16]
    
    return bytes(pt)

def make_ext2_with_files(file_list, total_disk_sectors, partition_lba):
    """ext2ファイルシステムをバイト列として生成し、ファイルを含める"""
    
    # ext2パーティションサイズ (LBA partition_lba以降)
    part_sectors = total_disk_sectors - partition_lba
    total_blocks = (part_sectors * SECTOR_SIZE) // EXT2_BLOCK_SIZE
    
    blocks_per_group = 8192
    inodes_per_group = 1024
    inode_table_blocks = (inodes_per_group * EXT2_INODE_SIZE) // EXT2_BLOCK_SIZE  # 128
    first_data_block_num = 5 + inode_table_blocks  # 133
    
    # ファイルごとにブロックを割り当て
    next_data_block = first_data_block_num + 1  # +1 for root dir
    next_inode = 12  # 11以下は予約
    
    file_entries = []  # (inode_num, name, data, block_list)
    
    for name, data in file_list:
        blocks_needed = (len(data) + EXT2_BLOCK_SIZE - 1) // EXT2_BLOCK_SIZE
        if blocks_needed == 0:
            blocks_needed = 1
        
        block_list = list(range(next_data_block, next_data_block + blocks_needed))
        file_entries.append((next_inode, name, data, block_list))
        
        next_data_block += blocks_needed
        next_inode += 1
    
    used_blocks_count = next_data_block
    used_inodes_count = next_inode - 1
    
    now = int(time.time())
    
    # === スーパーブロック ===
    sb = bytearray(EXT2_BLOCK_SIZE)
    struct.pack_into('<I', sb, 0,  inodes_per_group)        # s_inodes_count
    struct.pack_into('<I', sb, 4,  total_blocks)            # s_blocks_count
    struct.pack_into('<I', sb, 8,  total_blocks // 20)      # s_r_blocks_count
    struct.pack_into('<I', sb, 12, total_blocks - used_blocks_count)  # s_free_blocks
    struct.pack_into('<I', sb, 16, inodes_per_group - used_inodes_count)  # s_free_inodes
    struct.pack_into('<I', sb, 20, 1)                       # s_first_data_block
    struct.pack_into('<I', sb, 24, 0)                       # s_log_block_size (0=1KB)
    struct.pack_into('<I', sb, 28, 0)                       # s_log_frag_size
    struct.pack_into('<I', sb, 32, blocks_per_group)        # s_blocks_per_group
    struct.pack_into('<I', sb, 36, blocks_per_group)        # s_frags_per_group
    struct.pack_into('<I', sb, 40, inodes_per_group)        # s_inodes_per_group
    struct.pack_into('<I', sb, 44, now)                     # s_mtime
    struct.pack_into('<I', sb, 48, now)                     # s_wtime
    struct.pack_into('<H', sb, 52, 0)                       # s_mnt_count
    struct.pack_into('<H', sb, 54, 20)                      # s_max_mnt_count
    struct.pack_into('<H', sb, 56, EXT2_SUPER_MAGIC)        # s_magic
    struct.pack_into('<H', sb, 58, 1)                       # s_state
    struct.pack_into('<H', sb, 60, 1)                       # s_errors
    struct.pack_into('<I', sb, 64, now)                     # s_lastcheck
    struct.pack_into('<I', sb, 76, 0)                       # s_rev_level
    struct.pack_into('<I', sb, 84, 11)                      # s_first_ino
    struct.pack_into('<H', sb, 88, EXT2_INODE_SIZE)         # s_inode_size
    sb[120:128] = b'OS32_HDD'                               # s_volume_name
    
    # === グループディスクリプタ ===
    gd = bytearray(EXT2_BLOCK_SIZE)
    struct.pack_into('<I', gd, 0, 3)     # bg_block_bitmap
    struct.pack_into('<I', gd, 4, 4)     # bg_inode_bitmap
    struct.pack_into('<I', gd, 8, 5)     # bg_inode_table
    free_blk = min(blocks_per_group - used_blocks_count, 65535)
    struct.pack_into('<H', gd, 12, free_blk)               # bg_free_blocks
    struct.pack_into('<H', gd, 14, inodes_per_group - used_inodes_count)  # bg_free_inodes
    struct.pack_into('<H', gd, 16, 1)                      # bg_used_dirs (root only)
    
    # === ブロックビットマップ ===
    block_bitmap = bytearray(EXT2_BLOCK_SIZE)
    for i in range(used_blocks_count):
        block_bitmap[i // 8] |= (1 << (i % 8))
    
    # === iノードビットマップ ===
    inode_bitmap = bytearray(EXT2_BLOCK_SIZE)
    for i in range(used_inodes_count):
        inode_bitmap[i // 8] |= (1 << (i % 8))
    
    # === iノードテーブル ===
    inode_table = bytearray(inode_table_blocks * EXT2_BLOCK_SIZE)
    
    # ルートディレクトリiノード (inode 2, index 1)
    root_inode = bytearray(EXT2_INODE_SIZE)
    struct.pack_into('<H', root_inode, 0, S_IFDIR | 0o755)
    struct.pack_into('<I', root_inode, 4, EXT2_BLOCK_SIZE)
    struct.pack_into('<I', root_inode, 8, now)
    struct.pack_into('<I', root_inode, 12, now)
    struct.pack_into('<I', root_inode, 16, now)
    struct.pack_into('<H', root_inode, 26, 2 + len([e for e in file_entries if e[1].endswith('/')]))  # links
    struct.pack_into('<I', root_inode, 28, 2)  # i_blocks (512B単位)
    struct.pack_into('<I', root_inode, 40, first_data_block_num)  # block ptr[0]
    inode_table[1*EXT2_INODE_SIZE : 2*EXT2_INODE_SIZE] = root_inode
    
    # ファイルiノード
    for ino, name, data, blocks in file_entries:
        fi = bytearray(EXT2_INODE_SIZE)
        struct.pack_into('<H', fi, 0, S_IFREG | 0o644)
        struct.pack_into('<I', fi, 4, len(data))
        struct.pack_into('<I', fi, 8, now)
        struct.pack_into('<I', fi, 12, now)
        struct.pack_into('<I', fi, 16, now)
        struct.pack_into('<H', fi, 26, 1)  # links
        
        # 直接ブロックポインタ (最大12本)
        for bi, bp in enumerate(blocks[:12]):
            struct.pack_into('<I', fi, 40 + bi * 4, bp)
        
        # 間接ブロックが必要な場合 (12ブロック以上)
        total_i_blocks = len(blocks) * 2  # i_blocks は512B単位
        if len(blocks) > 12:
            # 間接ブロックを割り当て
            ind_block_num = next_data_block
            next_data_block += 1
            total_i_blocks += 2  # 間接ブロック自体も i_blocks に加算
            
            # i_block[12] = 間接ブロック番号
            struct.pack_into('<I', fi, 40 + 12 * 4, ind_block_num)
            
            # 間接ブロックのビットマップを更新
            block_bitmap[ind_block_num // 8] |= (1 << (ind_block_num % 8))
            
            # 間接ブロックデータを構築 (ブロック12以降のポインタ配列)
            ind_data = bytearray(EXT2_BLOCK_SIZE)
            for bi, bp in enumerate(blocks[12:]):
                struct.pack_into('<I', ind_data, bi * 4, bp)
            
            # 間接ブロックをput_block用にキューに記録
            if not hasattr(make_ext2_with_files, '_indirect_blocks'):
                make_ext2_with_files._indirect_blocks = []
            make_ext2_with_files._indirect_blocks.append((ind_block_num, ind_data))
        
        struct.pack_into('<I', fi, 28, total_i_blocks)  # i_blocks
        idx = (ino - 1) * EXT2_INODE_SIZE
        inode_table[idx:idx+EXT2_INODE_SIZE] = fi
    
    # === ルートディレクトリデータブロック ===
    root_dir = bytearray(EXT2_BLOCK_SIZE)
    pos = 0
    
    # "." エントリ
    de = bytearray(12)
    struct.pack_into('<I', de, 0, EXT2_ROOT_INO)
    struct.pack_into('<H', de, 4, 12)
    de[6] = 1; de[7] = 2  # name_len=1, type=DIR
    de[8] = ord('.')
    root_dir[pos:pos+12] = de
    pos += 12
    
    # ".." エントリ
    de2 = bytearray(12)
    struct.pack_into('<I', de2, 0, EXT2_ROOT_INO)
    struct.pack_into('<H', de2, 4, 12)
    de2[6] = 2; de2[7] = 2
    de2[8] = ord('.'); de2[9] = ord('.')
    root_dir[pos:pos+12] = de2
    pos += 12
    
    # ファイルエントリ
    for i, (ino, name, data, blocks) in enumerate(file_entries):
        name_bytes = name.encode('ascii')
        name_len = len(name_bytes)
        rec_len = (8 + name_len + 3) & ~3
        
        # 最後のエントリはブロック末尾まで
        if i == len(file_entries) - 1:
            rec_len = EXT2_BLOCK_SIZE - pos
        
        de_f = bytearray(rec_len)
        struct.pack_into('<I', de_f, 0, ino)
        struct.pack_into('<H', de_f, 4, rec_len)
        de_f[6] = name_len
        de_f[7] = 1  # EXT2_FT_REG_FILE
        de_f[8:8+name_len] = name_bytes
        root_dir[pos:pos+rec_len] = de_f
        pos += rec_len
    
    # === 全ブロックを組み立て ===
    # パーティション内のバイト配列を構築
    part_size = total_blocks * EXT2_BLOCK_SIZE
    partition = bytearray(part_size)
    
    def put_block(block_num, data):
        off = block_num * EXT2_BLOCK_SIZE
        partition[off:off+len(data)] = data
    
    put_block(1, sb)
    put_block(2, gd)
    put_block(3, block_bitmap)
    put_block(4, inode_bitmap)
    # iノードテーブル (ブロック5以降)
    partition[5*EXT2_BLOCK_SIZE : 5*EXT2_BLOCK_SIZE + len(inode_table)] = inode_table
    
    # ルートディレクトリ
    put_block(first_data_block_num, root_dir)
    
    # ファイルデータブロック
    for ino, name, data, blocks in file_entries:
        for bi, block_num in enumerate(blocks):
            chunk_start = bi * EXT2_BLOCK_SIZE
            chunk_end = min(chunk_start + EXT2_BLOCK_SIZE, len(data))
            if chunk_start < len(data):
                chunk = data[chunk_start:chunk_end]
                blk_data = bytearray(EXT2_BLOCK_SIZE)
                blk_data[:len(chunk)] = chunk
                put_block(block_num, blk_data)
    
    # 間接ブロック書き込み
    if hasattr(make_ext2_with_files, '_indirect_blocks'):
        for ind_num, ind_data in make_ext2_with_files._indirect_blocks:
            put_block(ind_num, ind_data)
        # used_blocks_countをリフレッシュ (ビットマップ / スーパーブロック再計算)
        actual_used = next_data_block
        free_blk = min(blocks_per_group - actual_used, 65535)
        struct.pack_into('<I', sb, 12, total_blocks - actual_used)  # s_free_blocks
        struct.pack_into('<H', gd, 12, free_blk)  # bg_free_blocks
        # ビットマップとスーパーブロックを再書き込み
        put_block(1, sb)
        put_block(2, gd)
        put_block(3, block_bitmap)
        del make_ext2_with_files._indirect_blocks
    
    return partition

def main():
    print("=" * 50)
    print("  OS32 HDD 直接インストーラー")
    print("=" * 50)
    
    # バイナリファイルのパス
    boot_hdd = os.path.join(PROJ_DIR, 'boot', 'test_p_boot.bin')
    loader_hdd = os.path.join(PROJ_DIR, 'boot', 'test_p_load.bin')
    kernel_bin = os.path.join(PROJ_DIR, 'kernel.bin')
    
    for f in [boot_hdd, loader_hdd, kernel_bin]:
        if not os.path.exists(f):
            print(f"エラー: {f} が見つかりません")
            sys.exit(1)
        print(f"  {os.path.basename(f)}: {os.path.getsize(f)} bytes")
    
    # プログラムファイルを収集
    prog_dir = os.path.join(PROJ_DIR, 'programs')
    file_list = []  # (name, data)
    
    for p in sorted(glob.glob(os.path.join(prog_dir, '*.bin'))):
        basename = os.path.basename(p).lower()
        with open(p, 'rb') as pf:
            file_list.append((basename, pf.read()))
        print(f"  {basename}: {os.path.getsize(p)} bytes")
    
    # テスト・辞書ファイル
    for extra in ['programs/test.txt', 'programs/test_utf8.txt', 'assets/SKK.LZS']:
        epath = os.path.join(PROJ_DIR, extra)
        if os.path.exists(epath):
            ename = os.path.basename(epath).lower()
            with open(epath, 'rb') as ef:
                file_list.append((ename, ef.read()))
            print(f"  {ename}: {os.path.getsize(epath)} bytes")
    
    print(f"\n合計 {len(file_list)} ファイル")
    
    # NHD読み込み
    with open(NHD_PATH, 'r+b') as f:
        geo = read_nhd_geometry(f)
        nhd_header = geo['header_size']
        total_sectors = geo['cylinders'] * geo['heads'] * geo['sectors']
        
        print(f"\nNHD: C={geo['cylinders']} H={geo['heads']} S={geo['sectors']} ({total_sectors} sectors)")
        
        # === Phase 1: IPL (LBA 0) ===
        with open(boot_hdd, 'rb') as bf:
            ipl_data = bytearray(bf.read())
        # ジオメトリパッチ
        ipl_data[8] = geo['heads']
        ipl_data[9] = geo['sectors']
        # パディング
        ipl_data = bytes(ipl_data).ljust(512, b'\x00')
        write_sector(f, nhd_header, 0, ipl_data)
        print("  [OK] IPL (LBA 0)")
        
        # === Phase 2: パーティションテーブル (LBA 1) ===
        pt = make_partition_table(geo)
        write_sector(f, nhd_header, 1, pt)
        print("  [OK] パーティションテーブル (LBA 1)")
        
        # === Phase 3: ローダー (LBA 2) ===
        with open(loader_hdd, 'rb') as lf:
            loader_data = bytearray(lf.read())
        # ジオメトリパッチ (offset 3: heads, 4: spt)
        loader_data[3] = geo['heads']
        loader_data[4] = geo['sectors']
        write_sectors(f, nhd_header, 2, bytes(loader_data))
        print(f"  [OK] ローダー (LBA 2, {len(loader_data)} bytes)")
        
        # === Phase 4: カーネル (LBA 6) ===
        with open(kernel_bin, 'rb') as kf:
            kernel_data = kf.read()
        write_sectors(f, nhd_header, 6, kernel_data)
        kernel_sects = (len(kernel_data) + 511) // 512
        print(f"  [OK] カーネル (LBA 6, {len(kernel_data)} bytes, {kernel_sects} sectors)")
        
        # === Phase 5: ext2ファイルシステム (LBA 136以降) ===
        print(f"\next2フォーマット中 (LBA {PARTITION_LBA} 以降)...")
        ext2_data = make_ext2_with_files(file_list, total_sectors, PARTITION_LBA)
        
        f.seek(nhd_header + PARTITION_LBA * SECTOR_SIZE)
        f.write(ext2_data)
        print(f"  [OK] ext2 ({len(ext2_data)} bytes)")
        
        # ファイル一覧
        print(f"\n=== インストール済みファイル ===")
        for name, data in file_list:
            print(f"  /{name} ({len(data)} bytes)")
    
    print(f"\n{'='*50}")
    print("  インストール完了!")
    print(f"  NHD: {NHD_PATH}")
    print(f"{'='*50}")

if __name__ == '__main__':
    if len(sys.argv) > 1:
        NHD_PATH = sys.argv[1]
    main()
