#!/usr/bin/env python3
"""
mkos32x.py - フラットバイナリに OS32X ヘッダを付加する

使い方:
    python3 mkos32x.py input.bin output.bin [オプション]

オプション:
    --elf FILE     生成したELFファイルからBSSサイズを自動検出
    --heap SIZE    要求ヒープサイズ (バイト, デフォルト: 0=64KB)
    --bss SIZE     BSSサイズ (バイト, 手動指定。--elfと併用時はelfが優先)
    --api VER      最低APIバージョン (デフォルト: 1)
    --entry OFF    エントリポイントオフセット (デフォルト: 0)
    --gfx          GFXフラグを設定
"""

import sys
import struct
import os
import re

OS32X_MAGIC = 0x4F533332  # 'OS32'
OS32X_HDR_V1_SIZE = 40
OS32X_FLAG_GFX = 0x0001


import subprocess

def parse_elf_bss(elf_path):
    """ELFファイルから .bss セグメントサイズを取得 (size -A コマンド使用)"""
    try:
        out = subprocess.check_output(['size', '-A', elf_path], text=True, errors='ignore')
        for line in out.splitlines():
            if line.startswith('.bss '):
                # .bss        256     4194404  などの形式から数値を抽出
                parts = line.split()
                if len(parts) >= 2:
                    return int(parts[1])
        return 0
    except Exception as e:
        print(f"  警告: ELFのBSS読み取りに失敗しました ({elf_path}): {e}")
        return 0


def parse_elf_entry(elf_path):
    """ELFファイルからエントリポイントのオフセットを取得する。

    ELFヘッダの e_entry (仮想アドレス) と .text セクションの開始アドレスの
    差分を計算し、バイナリ先頭からのオフセットを返す。
    """
    try:
        with open(elf_path, 'rb') as f:
            # ELFヘッダ (32bit)
            ident = f.read(16)
            if ident[:4] != b'\x7fELF':
                print(f"  警告: ELF形式ではありません ({elf_path})")
                return None
            ei_class = ident[4]  # 1=32bit, 2=64bit
            ei_data = ident[5]   # 1=LE, 2=BE
            if ei_class != 1:
                print(f"  警告: 32bit ELFのみサポート ({elf_path})")
                return None
            fmt = '<' if ei_data == 1 else '>'

            # e_type(2) + e_machine(2) + e_version(4) + e_entry(4)
            hdr = f.read(8)  # e_type, e_machine, e_version の先頭まで
            e_entry = struct.unpack(fmt + 'I', f.read(4))[0]

            # e_phoff(4) + e_shoff(4) + e_flags(4) + e_ehsize(2)
            # + e_phentsize(2) + e_phnum(2) + e_shentsize(2) + e_shnum(2) + e_shstrndx(2)
            f.read(4)   # e_phoff
            e_shoff = struct.unpack(fmt + 'I', f.read(4))[0]
            f.read(4)   # e_flags
            f.read(2)   # e_ehsize
            f.read(2)   # e_phentsize
            f.read(2)   # e_phnum
            e_shentsize = struct.unpack(fmt + 'H', f.read(2))[0]
            e_shnum = struct.unpack(fmt + 'H', f.read(2))[0]
            e_shstrndx = struct.unpack(fmt + 'H', f.read(2))[0]

            # セクションヘッダテーブルを読み込み
            f.seek(e_shoff)
            sections = []
            for _ in range(e_shnum):
                sh_data = f.read(e_shentsize)
                # sh_name(4) sh_type(4) sh_flags(4) sh_addr(4) sh_offset(4) ...
                sh_name, sh_type, sh_flags, sh_addr = struct.unpack(fmt + 'IIII', sh_data[:16])
                sections.append((sh_name, sh_type, sh_flags, sh_addr))

            # セクション名文字列テーブル
            if e_shstrndx < e_shnum:
                str_sh = sections[e_shstrndx]
                f.seek(e_shoff + e_shstrndx * e_shentsize + 16)  # sh_offset の位置
                str_offset = struct.unpack(fmt + 'I', f.read(4))[0]
                f.seek(str_offset)
                strtab = f.read(4096)  # 十分なサイズを読む
            else:
                strtab = b''

            # .text セクションの仮想アドレスを探す
            text_addr = None
            for sh_name, sh_type, sh_flags, sh_addr in sections:
                if sh_name < len(strtab):
                    end = strtab.index(b'\x00', sh_name) if b'\x00' in strtab[sh_name:] else sh_name
                    name = strtab[sh_name:end].decode('ascii', errors='ignore')
                    if name == '.text' or name == '.text.startup':
                        if text_addr is None or sh_addr < text_addr:
                            text_addr = sh_addr

            if text_addr is None:
                print(f"  警告: .text セクションが見つかりません ({elf_path})")
                return None

            entry_off = e_entry - text_addr
            if entry_off < 0:
                print(f"  警告: エントリポイントが .text より前にあります ({elf_path})")
                return None

            return entry_off

    except Exception as e:
        print(f"  警告: ELFエントリポイント読み取りに失敗 ({elf_path}): {e}")
        return None


def main():
    if len(sys.argv) < 3:
        print("使い方: mkos32x.py <input.bin> <output.bin> [options]")
        print("  --elf FILE    ELFファイルからBSSサイズ自動検出")
        print("  --heap SIZE   要求ヒープサイズ (バイト)")
        print("  --bss SIZE    BSSサイズ (バイト)")
        print("  --api VER     最低APIバージョン")
        print("  --entry OFF   エントリポイントオフセット")
        print("  --gfx         GFXフラグ設定")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    # オプション解析
    heap_size = 0
    bss_size = 0
    min_api_ver = 1
    entry_offset = 0
    flags = 0
    elf_path = None

    i = 3
    while i < len(sys.argv):
        if sys.argv[i] == '--heap' and i + 1 < len(sys.argv):
            heap_size = int(sys.argv[i + 1], 0)
            i += 2
        elif sys.argv[i] == '--bss' and i + 1 < len(sys.argv):
            bss_size = int(sys.argv[i + 1], 0)
            i += 2
        elif sys.argv[i] == '--api' and i + 1 < len(sys.argv):
            min_api_ver = int(sys.argv[i + 1], 0)
            i += 2
        elif sys.argv[i] == '--entry' and i + 1 < len(sys.argv):
            entry_offset = int(sys.argv[i + 1], 0)
            i += 2
        elif sys.argv[i] == '--elf' and i + 1 < len(sys.argv):
            elf_path = sys.argv[i + 1]
            i += 2
        elif sys.argv[i] == '--gfx':
            flags |= OS32X_FLAG_GFX
            i += 1
        else:
            print(f"不明なオプション: {sys.argv[i]}")
            sys.exit(1)

    # ELFファイルからBSSサイズ自動検出 (--bss手動指定より優先)
    if elf_path:
        elf_bss = parse_elf_bss(elf_path)
        if elf_bss > 0:
            bss_size = elf_bss

    # ELFファイルからエントリポイント自動取得
    # --entry で手動指定されていなければ ELF の値を使用
    if elf_path and entry_offset == 0:
        elf_entry = parse_elf_entry(elf_path)
        if elf_entry is not None:
            entry_offset = elf_entry
            if entry_offset != 0:
                print(f"  注意: ELFからエントリオフセット自動検出: 0x{entry_offset:X}")

    # 入力ファイル読み込み
    with open(input_path, 'rb') as f:
        code_data = f.read()

    text_size = len(code_data)

    # ヘッダ構築 (40バイト = 10 x u32)
    header = struct.pack('<IIIIIIIIII',
        OS32X_MAGIC,        # magic
        OS32X_HDR_V1_SIZE,  # header_size
        1,                  # version
        flags,              # flags
        entry_offset,       # entry_offset
        text_size,          # text_size
        bss_size,           # bss_size
        heap_size,          # heap_size
        0,                  # stack_size (予約)
        min_api_ver,        # min_api_ver
    )

    assert len(header) == OS32X_HDR_V1_SIZE, f"Header size mismatch: {len(header)}"

    # 出力
    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(code_data)

    total = OS32X_HDR_V1_SIZE + text_size
    print(f"  OS32X: {os.path.basename(output_path)} "
          f"(text={text_size}, bss={bss_size}, heap={heap_size}, "
          f"entry=0x{entry_offset:X}, api>={min_api_ver}, total={total})")

if __name__ == '__main__':
    main()
