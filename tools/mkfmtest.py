#!/usr/bin/env python3
"""
mkfmtest.py — PC-98 FM/SSGテスト用D88イメージ生成

Usage: python3 tools/mkfmtest.py
Output: /mnt/c/os32/fmtest.d88

1. NASMでfmtest.asmをアセンブル
2. IPLセクタとしてD88ディスクイメージに格納
"""

import struct
import subprocess
import os
import sys

NASM = "nasm"
ASM_SRC = os.path.join(os.path.dirname(__file__), "fmtest.asm")
BIN_OUT = "/tmp/fmtest.bin"
D88_OUT = "/mnt/c/os32/fmtest.d88"

# D88ディスクパラメータ (2DD: 80cyl x 2head x 16sec x 256bytes)
NUM_CYLINDERS = 80
NUM_HEADS = 2
SECTORS_PER_TRACK = 16
SECTOR_SIZE = 256
MEDIA_TYPE = 0x10  # 2DD

D88_HEADER_SIZE = 0x2B0  # 688 bytes
TRACK_TABLE_ENTRIES = 164
SECTOR_HEADER_SIZE = 0x10  # 16 bytes per sector


def assemble():
    """NASMでアセンブル"""
    cmd = [NASM, "-f", "bin", "-o", BIN_OUT, ASM_SRC]
    print(f"Assembling: {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"NASM error:\n{result.stderr}")
        sys.exit(1)
    size = os.path.getsize(BIN_OUT)
    print(f"  Binary size: {size} bytes")
    if size > SECTOR_SIZE:
        print(f"  WARNING: IPL exceeds {SECTOR_SIZE} bytes!")
    return open(BIN_OUT, "rb").read()


def make_d88(ipl_data):
    """D88ディスクイメージを生成"""
    num_tracks = NUM_CYLINDERS * NUM_HEADS
    track_data_size = SECTORS_PER_TRACK * (SECTOR_HEADER_SIZE + SECTOR_SIZE)
    disk_size = D88_HEADER_SIZE + num_tracks * track_data_size

    # ヘッダ
    name = b"FM TEST\x00" + b"\x00" * 9  # 17 bytes
    reserved = b"\x00" * 9
    write_protect = b"\x00"
    media_type = struct.pack("<B", MEDIA_TYPE)
    disk_size_bytes = struct.pack("<I", disk_size)

    # トラックオフセットテーブル
    track_offsets = bytearray(TRACK_TABLE_ENTRIES * 4)
    for t in range(num_tracks):
        offset = D88_HEADER_SIZE + t * track_data_size
        struct.pack_into("<I", track_offsets, t * 4, offset)

    header = name + reserved + write_protect + media_type + disk_size_bytes + bytes(track_offsets)
    assert len(header) == D88_HEADER_SIZE, f"Header size mismatch: {len(header)}"

    # トラックデータ
    all_tracks = bytearray()
    for track in range(num_tracks):
        cyl = track // NUM_HEADS
        head = track % NUM_HEADS
        for sec in range(SECTORS_PER_TRACK):
            # セクタヘッダ (16 bytes): C H R N count density del status size(2)
            sec_hdr = struct.pack("<BBBBHBBHIH",
                cyl,        # C
                head,       # H
                sec + 1,    # R (1-based)
                1,          # N (1=256bytes)
                SECTORS_PER_TRACK,  # number of sectors
                0,          # density (0=double)
                0,          # deleted mark
                0,          # status
                0,          # reserved (pad to 16)
                SECTOR_SIZE # actual data size
            )
            sec_hdr = sec_hdr[:SECTOR_HEADER_SIZE]

            # セクタデータ
            if cyl == 0 and head == 0 and sec == 0:
                # IPLセクタ
                data = ipl_data[:SECTOR_SIZE]
                data += b"\x00" * (SECTOR_SIZE - len(data))
            else:
                data = b"\x00" * SECTOR_SIZE

            all_tracks += sec_hdr + data

    # 書き込み
    with open(D88_OUT, "wb") as f:
        f.write(header)
        f.write(all_tracks)

    print(f"D88 image: {D88_OUT} ({os.path.getsize(D88_OUT)} bytes)")


def main():
    ipl = assemble()
    make_d88(ipl)
    print("Done!")


if __name__ == "__main__":
    main()
