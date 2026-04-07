#!/usr/bin/env python3
"""
create_ext2_hdd.py — ext2フォーマット済みHDDイメージ(NHD)を作成

NP21/Wで使用するため、既存のNHDファイルのセクタにext2構造を書き込む。
ext2のスーパーブロック、グループディスクリプタ、ブロックビットマップ、
iノードテーブル、ルートディレクトリを最小限で構成する。

使い方:
    python3 tools/create_ext2_hdd.py <nhd_file>

注意: NHDイメージの先頭512バイト(セクタ0)はIPLのため、
ext2はセクタ2 (オフセット1024) から開始する。
"""

import sys
import os
import struct
import time

# ext2定数
EXT2_SUPER_MAGIC = 0xEF53
EXT2_BLOCK_SIZE = 1024       # 1KBブロック
EXT2_INODE_SIZE = 128
EXT2_ROOT_INO = 2
EXT2_GOOD_OLD_REV = 0

# ファイルタイプ
S_IFDIR = 0o40000
S_IFREG = 0o100000

def make_superblock(total_blocks, blocks_per_group, inodes_per_group):
    """ext2スーパーブロック (1024バイト, オフセット1024)"""
    sb = bytearray(1024)
    
    total_inodes = inodes_per_group
    free_blocks = total_blocks - 10  # 予約ブロック差し引き
    free_inodes = total_inodes - 11  # 予約inode差し引き
    
    # スーパーブロックフィールド
    struct.pack_into('<I', sb, 0,   total_inodes)         # s_inodes_count
    struct.pack_into('<I', sb, 4,   total_blocks)         # s_blocks_count
    struct.pack_into('<I', sb, 8,   total_blocks // 20)   # s_r_blocks_count (5%)
    struct.pack_into('<I', sb, 12,  free_blocks)          # s_free_blocks_count
    struct.pack_into('<I', sb, 16,  free_inodes)          # s_free_inodes_count
    struct.pack_into('<I', sb, 20,  1)                    # s_first_data_block (1KB)
    struct.pack_into('<I', sb, 24,  0)                    # s_log_block_size (0=1KB)
    struct.pack_into('<I', sb, 28,  0)                    # s_log_frag_size
    struct.pack_into('<I', sb, 32,  blocks_per_group)     # s_blocks_per_group
    struct.pack_into('<I', sb, 36,  blocks_per_group)     # s_frags_per_group
    struct.pack_into('<I', sb, 40,  inodes_per_group)     # s_inodes_per_group
    struct.pack_into('<I', sb, 44,  int(time.time()))      # s_mtime
    struct.pack_into('<I', sb, 48,  int(time.time()))      # s_wtime
    struct.pack_into('<H', sb, 52,  0)                    # s_mnt_count
    struct.pack_into('<H', sb, 54,  20)                   # s_max_mnt_count
    struct.pack_into('<H', sb, 56,  EXT2_SUPER_MAGIC)     # s_magic
    struct.pack_into('<H', sb, 58,  1)                    # s_state (clean)
    struct.pack_into('<H', sb, 60,  1)                    # s_errors (continue)
    struct.pack_into('<I', sb, 64,  int(time.time()))      # s_lastcheck
    struct.pack_into('<I', sb, 68,  86400 * 180)          # s_checkinterval (180日)
    struct.pack_into('<I', sb, 72,  0)                    # s_creator_os (Linux)
    struct.pack_into('<I', sb, 76,  EXT2_GOOD_OLD_REV)    # s_rev_level
    struct.pack_into('<H', sb, 80,  0)                    # s_def_resuid
    struct.pack_into('<H', sb, 82,  0)                    # s_def_resgid
    struct.pack_into('<I', sb, 84,  11)                   # s_first_ino
    struct.pack_into('<H', sb, 88,  EXT2_INODE_SIZE)      # s_inode_size
    
    # ボリュームラベル (s_volume_name at offset 120)
    label = b'OS32_HDD'
    sb[120:120+len(label)] = label
    
    return sb

def make_group_descriptor(block_bitmap, inode_bitmap, inode_table, 
                          free_blocks, free_inodes, used_dirs):
    """グループディスクリプタ (32バイト)"""
    gd = bytearray(32)
    struct.pack_into('<I', gd, 0,  block_bitmap)    # bg_block_bitmap
    struct.pack_into('<I', gd, 4,  inode_bitmap)    # bg_inode_bitmap
    struct.pack_into('<I', gd, 8,  inode_table)     # bg_inode_table
    struct.pack_into('<H', gd, 12, free_blocks)     # bg_free_blocks_count
    struct.pack_into('<H', gd, 14, free_inodes)     # bg_free_inodes_count
    struct.pack_into('<H', gd, 16, used_dirs)       # bg_used_dirs_count
    return gd

def make_inode(mode, size, blocks, block_ptrs, mtime=None):
    """iノード (128バイト)"""
    inode = bytearray(EXT2_INODE_SIZE)
    if mtime is None:
        mtime = int(time.time())
    struct.pack_into('<H', inode, 0,  mode)          # i_mode
    struct.pack_into('<I', inode, 4,  size)           # i_size
    struct.pack_into('<I', inode, 8,  mtime)          # i_atime
    struct.pack_into('<I', inode, 12, mtime)          # i_ctime
    struct.pack_into('<I', inode, 16, mtime)          # i_mtime
    struct.pack_into('<H', inode, 26, 1)              # i_links_count
    struct.pack_into('<I', inode, 28, blocks * 2)     # i_blocks (512B単位)
    # ダイレクトブロックポインタ (offset 40)
    for i, bp in enumerate(block_ptrs[:12]):
        struct.pack_into('<I', inode, 40 + i * 4, bp)
    return inode

def make_dir_entry(inode, name, file_type):
    """ディレクトリエントリ"""
    name_bytes = name.encode('ascii')
    name_len = len(name_bytes)
    # rec_len: 8 + name_len, 4バイト境界に切り上げ
    rec_len = (8 + name_len + 3) & ~3
    entry = bytearray(rec_len)
    struct.pack_into('<I', entry, 0, inode)      # inode
    struct.pack_into('<H', entry, 4, rec_len)    # rec_len
    entry[6] = name_len                          # name_len
    entry[7] = file_type                         # file_type (1=REG, 2=DIR)
    entry[8:8+name_len] = name_bytes
    return entry

def format_ext2(nhd_path):
    """既存NHDファイルにext2を書き込み"""
    
    file_size = os.path.getsize(nhd_path)
    
    # NHDヘッダサイズ検出 (先頭に"T98HDDIMAGE"があるか)
    with open(nhd_path, 'rb') as f:
        header = f.read(512)
    
    nhd_header_size = 0
    if header[:11] == b'T98HDDIMAGE':
        nhd_header_size = 512
        print(f"NHDヘッダ検出 (512バイト)")
    
    disk_size = file_size - nhd_header_size
    total_blocks = disk_size // EXT2_BLOCK_SIZE
    
    print(f"ディスクサイズ: {disk_size} bytes ({disk_size // (1024*1024)} MB)")
    print(f"ブロック数: {total_blocks}")
    
    # ブロックグループレイアウト
    blocks_per_group = 8192  # 1KBブロック × 8192 = 8MB/グループ
    inodes_per_group = 1024  # グループあたりinode数
    
    # ブロック割り当て (最初のグループのみ):
    #   Block 0: (boot, 未使用 — スーパーブロックはblock 1)
    #   Block 1: スーパーブロック
    #   Block 2: グループディスクリプタテーブル
    #   Block 3: ブロックビットマップ
    #   Block 4: iノードビットマップ
    #   Block 5-132: iノードテーブル (1024 inodes × 128B = 128KB = 128ブロック)
    #   Block 133+: データブロック

    inode_table_blocks = (inodes_per_group * EXT2_INODE_SIZE) // EXT2_BLOCK_SIZE
    first_data_block = 5 + inode_table_blocks  # = 133
    
    print(f"iノードテーブル: {inode_table_blocks} ブロック")
    print(f"最初のデータブロック: {first_data_block}")
    
    # スーパーブロック
    sb = make_superblock(total_blocks, blocks_per_group, inodes_per_group)
    
    # グループディスクリプタ (パディングして1ブロック)
    free_blk = blocks_per_group - first_data_block - 1  # グループ0の空きブロック
    if free_blk > 65535:
        free_blk = 65535
    gd = make_group_descriptor(
        block_bitmap=3, inode_bitmap=4, inode_table=5,
        free_blocks=free_blk, free_inodes=inodes_per_group - 11 - 1, used_dirs=1
    )
    gd_block = bytearray(EXT2_BLOCK_SIZE)
    gd_block[:len(gd)] = gd
    
    # ブロックビットマップ
    block_bitmap = bytearray(EXT2_BLOCK_SIZE)
    # ブロック0～first_data_block + ルートDIRブロック を使用済みに
    used_blocks = first_data_block + 1  # +1 for root dir data block
    for i in range(used_blocks):
        block_bitmap[i // 8] |= (1 << (i % 8))
    
    # iノードビットマップ
    inode_bitmap = bytearray(EXT2_BLOCK_SIZE)
    # inode 1-11 を使用済み (1-10は予約, 11はlost+found用)
    for i in range(11):
        inode_bitmap[i // 8] |= (1 << (i % 8))
    # inode 12 = テストファイル用
    inode_bitmap[11 // 8] |= (1 << (11 % 8))
    
    # iノードテーブル
    inode_table = bytearray(inode_table_blocks * EXT2_BLOCK_SIZE)
    
    # ルートiノード (inode 2, インデックス1)
    root_inode = make_inode(
        mode=S_IFDIR | 0o755,
        size=EXT2_BLOCK_SIZE,
        blocks=1,
        block_ptrs=[first_data_block]
    )
    idx = 1 * EXT2_INODE_SIZE  # inode 2 = index 1
    inode_table[idx:idx+EXT2_INODE_SIZE] = root_inode
    
    # テストファイルiノード (inode 12, インデックス11)
    test_content = b'Hello from ext2 on IDE HDD!\nThis is OS32.\n'
    test_data_block = first_data_block + 1  # ルートDIRの次のブロック
    test_inode = make_inode(
        mode=S_IFREG | 0o644,
        size=len(test_content),
        blocks=1,
        block_ptrs=[test_data_block]
    )
    idx = 11 * EXT2_INODE_SIZE  # inode 12 = index 11
    inode_table[idx:idx+EXT2_INODE_SIZE] = test_inode
    
    # そのブロックもビットマップに
    block_bitmap[test_data_block // 8] |= (1 << (test_data_block % 8))
    
    # ルートディレクトリデータブロック
    root_dir = bytearray(EXT2_BLOCK_SIZE)
    pos = 0
    
    # "." エントリ
    dot = make_dir_entry(EXT2_ROOT_INO, '.', 2)
    root_dir[pos:pos+len(dot)] = dot
    pos += len(dot)
    
    # ".." エントリ
    dotdot = make_dir_entry(EXT2_ROOT_INO, '..', 2)
    root_dir[pos:pos+len(dotdot)] = dotdot
    pos += len(dotdot)
    
    # "hello.txt" エントリ
    hello = make_dir_entry(12, 'hello.txt', 1)
    # 最後のエントリのrec_lenはブロック末尾まで
    remaining = EXT2_BLOCK_SIZE - pos
    struct.pack_into('<H', hello, 4, remaining)  # rec_lenを残り全部に
    root_dir[pos:pos+len(hello)] = hello
    
    # テストファイルデータブロック
    test_block = bytearray(EXT2_BLOCK_SIZE)
    test_block[:len(test_content)] = test_content
    
    # 書き込み
    with open(nhd_path, 'r+b') as f:
        base = nhd_header_size
        
        # Block 1: スーパーブロック (オフセット 1024)
        f.seek(base + 1 * EXT2_BLOCK_SIZE)
        f.write(sb)
        
        # Block 2: グループディスクリプタ
        f.seek(base + 2 * EXT2_BLOCK_SIZE)
        f.write(gd_block)
        
        # Block 3: ブロックビットマップ
        f.seek(base + 3 * EXT2_BLOCK_SIZE)
        f.write(block_bitmap)
        
        # Block 4: iノードビットマップ
        f.seek(base + 4 * EXT2_BLOCK_SIZE)
        f.write(inode_bitmap)
        
        # Block 5-132: iノードテーブル
        f.seek(base + 5 * EXT2_BLOCK_SIZE)
        f.write(inode_table)
        
        # Block first_data_block: ルートディレクトリ
        f.seek(base + first_data_block * EXT2_BLOCK_SIZE)
        f.write(root_dir)
        
        # Block first_data_block+1: テストファイル
        f.seek(base + test_data_block * EXT2_BLOCK_SIZE)
        f.write(test_block)
    
    print(f"\next2フォーマット完了!")
    print(f"  ボリュームラベル: OS32_HDD")
    print(f"  ブロックサイズ: {EXT2_BLOCK_SIZE}")
    print(f"  総ブロック数: {total_blocks}")
    print(f"  iノード数: {inodes_per_group}")
    print(f"  テストファイル: /hello.txt ({len(test_content)} bytes)")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("使い方: python3 create_ext2_hdd.py <nhd_file>")
        sys.exit(1)
    format_ext2(sys.argv[1])
