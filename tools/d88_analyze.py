#!/usr/bin/env python3
"""d88_analyze.py - D88ディスクイメージ解析ツール

使い方:
  python3 d88_analyze.py <D88ファイル> <コマンド> [オプション]

コマンド:
  info              D88ヘッダ情報を表示
  multidisk         マルチディスク(連結)検出
  tracks            全トラックのオフセットとセクタ数を表示
  dump <offset> <len>  D88生オフセットからダンプ
                    例: dump 0x3C50 64
  ipl_dump <off> <len>  IPLセグメント内のオフセットからダンプ
                    例: ipl_dump 0x0930 32
  find <hex|ascii>  バイトパターンを検索
                    例: find DKMUS1  または  find b800cc
  disasm <seg_off> <len>  簡易逆アセンブル (x86-16)
                    例: disasm 0060:0930 32
"""

import sys
import struct
import os

D88_HEADER_SIZE = 0x2B0


class D88Image:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()

        # ヘッダ解析
        self.name = self.data[0:17].split(b'\x00')[0].decode('ascii', errors='replace')
        self.protect = self.data[0x1A]
        self.media_type = struct.unpack('<B', self.data[0x1B:0x1C])[0]
        self.disk_size = struct.unpack('<I', self.data[0x1C:0x20])[0]

        # トラックオフセットテーブル (最大164トラック)
        self.track_offsets = []
        for i in range(164):
            off = struct.unpack('<I', self.data[0x20 + i * 4: 0x24 + i * 4])[0]
            if off > 0:
                self.track_offsets.append((i, off))

        # セクタマップ構築: (C, H, R) -> (d88_offset, size)
        self.sector_map = {}
        self._build_sector_map()

        # IPLマップ: IPLセグメントオフセット → D88データオフセット
        self.ipl_map = None
        self._build_ipl_map()

    def _build_sector_map(self):
        """全セクタのマップを構築"""
        for track_idx, track_off in self.track_offsets:
            pos = track_off
            while pos < len(self.data) - 16:
                c = self.data[pos]
                h = self.data[pos + 1]
                r = self.data[pos + 2]
                n = self.data[pos + 3]
                nsec = struct.unpack('<H', self.data[pos + 4:pos + 6])[0]
                data_len = struct.unpack('<H', self.data[pos + 14:pos + 16])[0]

                self.sector_map[(c, h, r)] = (pos + 16, data_len)

                pos += 16 + data_len

                # 次のトラックに到達したら終了
                next_tracks = [o for _, o in self.track_offsets if o > track_off]
                if next_tracks and pos >= min(next_tracks):
                    break

    def _build_ipl_map(self):
        """IPLセグメント (0060:0000) のメモリレイアウトを構築
        
        PC-98 1MB FDDのIPLは通常 Track 0, Head 0 の全セクタを
        0060:0000 に連続ロードする。
        """
        ipl_data = bytearray()
        ipl_offsets = []  # (mem_offset, d88_offset, size)

        # Track 0, Head 0 のセクタを番号順にロード
        t0_sectors = []
        for (c, h, r), (d88_off, size) in self.sector_map.items():
            if c == 0 and h == 0:
                t0_sectors.append((r, d88_off, size))
        t0_sectors.sort()

        mem_off = 0
        for r, d88_off, size in t0_sectors:
            ipl_offsets.append((mem_off, d88_off, size))
            ipl_data.extend(self.data[d88_off:d88_off + size])
            mem_off += size

        self.ipl_data = bytes(ipl_data)
        self.ipl_offsets = ipl_offsets

    def ipl_to_d88(self, ipl_offset):
        """IPLセグメント内オフセット → D88ファイルオフセットに変換"""
        running = 0
        for mem_off, d88_off, size in self.ipl_offsets:
            if mem_off <= ipl_offset < mem_off + size:
                return d88_off + (ipl_offset - mem_off)
            running = mem_off + size
        return None

    def get_ipl_bytes(self, offset, length):
        """IPLセグメント内のバイト列を取得"""
        if offset + length <= len(self.ipl_data):
            return self.ipl_data[offset:offset + length]
        return None

    def seg_off_to_ipl(self, seg, off):
        """seg:off → IPLオフセット (seg=0060hの場合のみ)"""
        if seg == 0x0060:
            return off
        return None


def cmd_info(img):
    print(f"ファイル名: {img.name}")
    media = {0x00: '2D', 0x10: '2DD', 0x20: '2HD'}
    print(f"メディア: {media.get(img.media_type, f'不明(0x{img.media_type:02X})')}")
    print(f"ディスクサイズ: {img.disk_size} bytes ({img.disk_size // 1024} KB)")
    print(f"トラック数: {len(img.track_offsets)}")
    print(f"セクタ数: {len(img.sector_map)}")
    print(f"IPLサイズ: {len(img.ipl_data)} bytes")
    total = len(img.data)
    if img.disk_size < total:
        print(f"★ マルチディスク検出! (disk_size={img.disk_size}, file={total})")


def cmd_tracks(img):
    print(f"{'Track':>5} {'Offset':>10} {'セクタ':>6}")
    print("-" * 25)
    for track_idx, track_off in img.track_offsets[:20]:
        # このトラックのセクタ数をカウント
        nsec = sum(1 for (c, h, r) in img.sector_map
                   if c == track_idx // 2 and h == track_idx % 2)
        print(f"{track_idx:5d} 0x{track_off:08X} {nsec:6d}")
    if len(img.track_offsets) > 20:
        print(f"  ... (残り {len(img.track_offsets) - 20} トラック)")


def cmd_multidisk(data):
    """マルチディスク(連結D88)の検出と各ディスクの情報を表示"""
    total = len(data)
    offset = 0
    disk_num = 0
    while offset < total - 0x20:
        disk_num += 1
        name = data[offset:offset+17].split(b'\x00')[0].decode('ascii', errors='replace')
        if offset + 0x20 > total:
            break
        disk_size = struct.unpack('<I', data[offset+0x1C:offset+0x20])[0]
        media = data[offset+0x1B]
        media_names = {0x00: '2D', 0x10: '2DD', 0x20: '2HD'}
        # トラック数を数える
        ntrk = 0
        for i in range(164):
            t = struct.unpack('<I', data[offset+0x20+i*4:offset+0x24+i*4])[0]
            if t > 0:
                ntrk += 1
        print(f"Disk #{disk_num}: offset=0x{offset:X} size={disk_size} "
              f"({disk_size//1024}KB) media={media_names.get(media, f'0x{media:02X}')} "
              f"tracks={ntrk} name='{name}'")
        if disk_size == 0 or disk_size > total:
            print(f"  ★ 不正なdisk_size, 終了")
            break
        offset += disk_size
    remaining = total - offset
    if remaining > 0:
        print(f"\n残りデータ: {remaining} bytes at 0x{offset:X}")
    print(f"\n合計: {disk_num} ディスク")


def cmd_dump_raw(data, offset, length):
    """D88生オフセットからのヘックスダンプ"""
    if offset >= len(data):
        print(f"エラー: オフセット 0x{offset:X} は範囲外 (ファイルサイズ: 0x{len(data):X})")
        return
    end = min(offset + length, len(data))
    chunk = data[offset:end]
    for i in range(0, len(chunk), 16):
        addr = offset + i
        hexb = ' '.join(f'{b:02X}' for b in chunk[i:i+16])
        asc = ''.join(chr(b) if 0x20 <= b < 0x7f else '.' for b in chunk[i:i+16])
        print(f"  {addr:05X}: {hexb:<48s} {asc}")


def cmd_ipl_dump(img, offset, length):
    data = img.get_ipl_bytes(offset, length)
    if data is None:
        print(f"エラー: IPLオフセット 0x{offset:X} は範囲外")
        return

    d88_off = img.ipl_to_d88(offset)
    print(f"IPL 0x{offset:04X} (D88 0x{d88_off:05X}):")

    for i in range(0, len(data), 16):
        addr = offset + i
        hexb = ' '.join(f'{b:02X}' for b in data[i:i + 16])
        ascii_str = ''.join(chr(b) if 0x20 <= b < 0x7f else '.' for b in data[i:i + 16])
        print(f"  {addr:04X}: {hexb:<48s} {ascii_str}")


def cmd_find(img, pattern):
    """バイトパターン検索 (ASCII or HEX)"""
    try:
        pat = bytes.fromhex(pattern)
        pat_name = f"HEX:{pattern}"
    except ValueError:
        pat = pattern.encode('ascii')
        pat_name = f"ASCII:{pattern}"

    print(f"検索: {pat_name} ({len(pat)} bytes)")
    print()

    # D88全体を検索
    idx = 0
    count = 0
    while count < 30:
        idx = img.data.find(pat, idx)
        if idx < 0:
            break
        ctx = img.data[max(0, idx - 4):idx + len(pat) + 4]
        print(f"  D88 0x{idx:05X}: {ctx.hex()}")
        idx += 1
        count += 1

    if count == 0:
        print("  見つかりませんでした")
    elif count >= 30:
        print(f"  ... (30件で打ち切り)")

    # IPL内も検索
    print()
    print("IPL内:")
    idx = 0
    ipl_count = 0
    while ipl_count < 20:
        idx = img.ipl_data.find(pat, idx)
        if idx < 0:
            break
        d88_off = img.ipl_to_d88(idx)
        ctx = img.ipl_data[max(0, idx - 4):idx + len(pat) + 4]
        print(f"  IPL 0x{idx:04X} (D88 0x{d88_off:05X}): {ctx.hex()}")
        idx += 1
        ipl_count += 1

    if ipl_count == 0:
        print("  見つかりませんでした")


def cmd_disasm(img, seg, off, length):
    """簡易逆アセンブリ (最小限のx86-16デコーダ)"""
    if seg == 0x0060:
        data = img.get_ipl_bytes(off, length)
        if data is None:
            print(f"エラー: 範囲外")
            return
    else:
        print(f"注意: seg=0x{seg:04X} はIPL(0060)外。D88ファイル内のバイトを直接使用。")
        return

    print(f"逆アセンブル: {seg:04X}:{off:04X} ({length} bytes)")
    print()
    i = 0
    while i < len(data):
        addr = off + i
        b = data[i]
        # 超簡易デコーダ (主要な命令のみ)
        raw_start = i
        desc = ""

        if b == 0xCD and i + 1 < len(data):
            desc = f"INT 0x{data[i+1]:02X}"
            i += 2
        elif b == 0xB8 and i + 2 < len(data):
            w = data[i + 1] | (data[i + 2] << 8)
            desc = f"MOV AX, 0x{w:04X}"
            i += 3
        elif b == 0xB0 and i + 1 < len(data):
            desc = f"MOV AL, 0x{data[i+1]:02X}"
            i += 2
        elif b == 0xB4 and i + 1 < len(data):
            desc = f"MOV AH, 0x{data[i+1]:02X}"
            i += 2
        elif b == 0x8E and i + 1 < len(data):
            rm = data[i + 1]
            regs = ['ES', 'CS', 'SS', 'DS']
            reg = regs[(rm >> 3) & 3]
            src = ['AX', 'CX', 'DX', 'BX', 'SP', 'BP', 'SI', 'DI'][rm & 7]
            desc = f"MOV {reg}, {src}"
            i += 2
        elif b == 0x26:
            desc = "ES:"
            i += 1
            continue  # プレフィックスのみ、次の命令と結合
        elif b == 0xA1 and i + 2 < len(data):
            w = data[i + 1] | (data[i + 2] << 8)
            desc = f"MOV AX, [{w:04X}h]"
            i += 3
        elif b == 0xA0 and i + 2 < len(data):
            w = data[i + 1] | (data[i + 2] << 8)
            desc = f"MOV AL, [{w:04X}h]"
            i += 3
        elif b == 0xA2 and i + 2 < len(data):
            w = data[i + 1] | (data[i + 2] << 8)
            desc = f"MOV [{w:04X}h], AL"
            i += 3
        elif b == 0xA8 and i + 1 < len(data):
            desc = f"TEST AL, 0x{data[i+1]:02X}"
            i += 2
        elif b == 0x3C and i + 1 < len(data):
            desc = f"CMP AL, 0x{data[i+1]:02X}"
            i += 2
        elif b == 0x74 and i + 1 < len(data):
            rel = data[i + 1]
            if rel >= 0x80:
                rel -= 0x100
            target = addr + 2 + rel
            desc = f"JZ 0x{target:04X}"
            i += 2
        elif b == 0x75 and i + 1 < len(data):
            rel = data[i + 1]
            if rel >= 0x80:
                rel -= 0x100
            target = addr + 2 + rel
            desc = f"JNZ 0x{target:04X}"
            i += 2
        elif b == 0xBA and i + 2 < len(data):
            w = data[i + 1] | (data[i + 2] << 8)
            desc = f"MOV DX, 0x{w:04X}"
            i += 3
        elif b == 0xEE:
            desc = "OUT DX, AL"
            i += 1
        elif b == 0xEC:
            desc = "IN AL, DX"
            i += 1
        elif b == 0xC3:
            desc = "RET"
            i += 1
        elif b == 0x06:
            desc = "PUSH ES"
            i += 1
        elif b == 0x07:
            desc = "POP ES"
            i += 1
        elif b == 0xE8 and i + 2 < len(data):
            rel = data[i + 1] | (data[i + 2] << 8)
            if rel >= 0x8000:
                rel -= 0x10000
            target = addr + 3 + rel
            desc = f"CALL 0x{target:04X}"
            i += 3
        elif b == 0xE9 and i + 2 < len(data):
            rel = data[i + 1] | (data[i + 2] << 8)
            if rel >= 0x8000:
                rel -= 0x10000
            target = addr + 3 + rel
            desc = f"JMP 0x{target:04X}"
            i += 3
        elif b == 0xEB and i + 1 < len(data):
            rel = data[i + 1]
            if rel >= 0x80:
                rel -= 0x100
            target = addr + 2 + rel
            desc = f"JMP SHORT 0x{target:04X}"
            i += 2
        elif b == 0x83 and i + 2 < len(data):
            rm = data[i + 1]
            imm = data[i + 2]
            ops = ['ADD', 'OR', 'ADC', 'SBB', 'AND', 'SUB', 'XOR', 'CMP']
            op = ops[(rm >> 3) & 7]
            regs16 = ['AX', 'CX', 'DX', 'BX', 'SP', 'BP', 'SI', 'DI']
            if (rm & 0xC0) == 0xC0:
                reg = regs16[rm & 7]
                desc = f"{op} {reg}, 0x{imm:02X}"
            else:
                desc = f"{op} r/m, 0x{imm:02X}"
            i += 3
        elif b == 0xFA:
            desc = "CLI"
            i += 1
        elif b == 0xFB:
            desc = "STI"
            i += 1
        elif b == 0xFC:
            desc = "CLD"
            i += 1
        elif b == 0xF3:
            desc = "REP"
            i += 1
            continue
        elif b == 0xAB:
            desc = "STOSW"
            i += 1
        else:
            desc = f"DB 0x{b:02X}"
            i += 1

        raw = data[raw_start:i]
        raw_hex = ' '.join(f'{x:02X}' for x in raw)
        print(f"  {addr:04X}: {raw_hex:<20s} {desc}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    d88_path = sys.argv[1]
    cmd = sys.argv[2]

    if not os.path.exists(d88_path):
        print(f"エラー: ファイルが見つかりません: {d88_path}")
        sys.exit(1)

    # multidiskとdumpはD88Image不要で生データを使う
    if cmd == 'multidisk':
        with open(d88_path, 'rb') as f:
            cmd_multidisk(f.read())
        return
    elif cmd == 'dump':
        if len(sys.argv) < 5:
            print("使い方: d88_analyze.py <file> dump <offset> <length>")
            sys.exit(1)
        with open(d88_path, 'rb') as f:
            raw = f.read()
        cmd_dump_raw(raw, int(sys.argv[3], 0), int(sys.argv[4], 0))
        return

    img = D88Image(d88_path)

    if cmd == 'info':
        cmd_info(img)
    elif cmd == 'tracks':
        cmd_tracks(img)
    elif cmd == 'ipl_dump':
        if len(sys.argv) < 5:
            print("使い方: d88_analyze.py <file> ipl_dump <offset> <length>")
            sys.exit(1)
        off = int(sys.argv[3], 0)
        length = int(sys.argv[4], 0)
        cmd_ipl_dump(img, off, length)
    elif cmd == 'find':
        if len(sys.argv) < 4:
            print("使い方: d88_analyze.py <file> find <pattern>")
            sys.exit(1)
        cmd_find(img, sys.argv[3])
    elif cmd == 'disasm':
        if len(sys.argv) < 5:
            print("使い方: d88_analyze.py <file> disasm <seg:off> <length>")
            sys.exit(1)
        parts = sys.argv[3].split(':')
        seg = int(parts[0], 16)
        off = int(parts[1], 16)
        length = int(sys.argv[4], 0)
        cmd_disasm(img, seg, off, length)
    else:
        print(f"不明なコマンド: {cmd}")
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
