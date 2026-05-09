#!/usr/bin/env python3
"""fdi_spec_parse.py — FDI仕様書準拠パーサ (ground truth)

Anex86 FDI フォーマットのパーサ。
ヘッダ (4096B) + RAW セクタデータという単純な構造。

使い方:
  python3 fdi_spec_parse.py <FDIファイル> <コマンド> [オプション]

コマンド:
  info                        ヘッダ情報を表示
  lba <N> [--count M]         LBA N から M セクタ分のデータをバイナリ出力
  extract-raw                 FDI → RAW 変換 (ヘッダ除去)
  compare <other.bin>         LBA全走査で他ツール出力と差分比較
"""

import sys
import struct
import os


class FDIImage:
    """FDI仕様書準拠のパーサ"""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()

        if len(self.data) < 4096:
            raise ValueError("ファイルサイズ不足: %d < 4096" % len(self.data))

        # ヘッダ解析 (先頭 32B, 全フィールド LE32)
        hdr = self.data[:32]
        self.dummy      = struct.unpack_from('<I', hdr, 0)[0]
        self.fddtype    = struct.unpack_from('<I', hdr, 4)[0]
        self.hdr_size   = struct.unpack_from('<I', hdr, 8)[0]
        self.fdd_size   = struct.unpack_from('<I', hdr, 12)[0]
        self.sect_size  = struct.unpack_from('<I', hdr, 16)[0]
        self.spt        = struct.unpack_from('<I', hdr, 20)[0]
        self.surfaces   = struct.unpack_from('<I', hdr, 24)[0]
        self.cyls       = struct.unpack_from('<I', hdr, 28)[0]

        # バリデーション
        if self.hdr_size < 32 or self.hdr_size > 65536:
            raise ValueError("不正な hdr_size: %d" % self.hdr_size)
        if self.sect_size == 0 or self.sect_size > 4096:
            raise ValueError("不正な sect_size: %d" % self.sect_size)
        if self.spt == 0 or self.spt > 64:
            raise ValueError("不正な spt: %d" % self.spt)

        self.total_lba = self.cyls * self.surfaces * self.spt

        # メディア種別名
        type_map = {
            0x10: '2DD(1MB/640KB)',
            0x30: '2HD(1.44MB)', 0xB0: '2HD(1.44MB)',
            0x50: '2D(320KB)',   0xD0: '2D(320KB)',
            0x70: '2DD(640KB)',  0xF0: '2DD(640KB)',
            0x90: '2HD(1.2MB)',
        }
        self.type_name = type_map.get(self.fddtype, 'unknown(0x%02X)' % self.fddtype)

    def read_lba(self, lba):
        """LBA で指定されたセクタのデータを返す"""
        if lba < 0 or lba >= self.total_lba:
            return None
        offset = self.hdr_size + lba * self.sect_size
        end = offset + self.sect_size
        if end > len(self.data):
            raw = self.data[offset:]
            raw += b'\x00' * (self.sect_size - len(raw))
            return raw
        return self.data[offset:end]

    def extract_raw(self):
        """FDI → RAW 変換 (ヘッダ除去)"""
        result = bytearray()
        for lba in range(self.total_lba):
            data = self.read_lba(lba)
            if data is None:
                result += b'\x00' * self.sect_size
            else:
                result += data
        return bytes(result)


def cmd_info(img):
    print("fddtype       : 0x%02X (%s)" % (img.fddtype, img.type_name))
    print("ヘッダサイズ  : %d bytes" % img.hdr_size)
    print("FDDサイズ     : %d bytes" % img.fdd_size)
    print("セクタサイズ  : %d bytes" % img.sect_size)
    print("SPT           : %d" % img.spt)
    print("ヘッド数      : %d" % img.surfaces)
    print("シリンダ数    : %d" % img.cyls)
    print("総LBA数       : %d" % img.total_lba)
    print("データ容量    : %d bytes (%d KB)" % (
        img.total_lba * img.sect_size,
        img.total_lba * img.sect_size // 1024))


def cmd_lba(img, start_lba, count):
    for i in range(count):
        lba = start_lba + i
        if lba >= img.total_lba:
            print("LBA %d は範囲外 (total=%d)" % (lba, img.total_lba),
                  file=sys.stderr)
            break
        data = img.read_lba(lba)
        if data is None:
            sys.stdout.buffer.write(b'\x00' * img.sect_size)
        else:
            sys.stdout.buffer.write(data)


def cmd_extract_raw(img):
    raw = img.extract_raw()
    sys.stdout.buffer.write(raw)


def cmd_compare(img, other_path):
    """LBA 全走査で他バイナリと差分比較"""
    with open(other_path, 'rb') as f:
        other = f.read()

    mismatches = 0
    first_mismatch = None

    for lba in range(img.total_lba):
        spec_data = img.read_lba(lba)
        if spec_data is None:
            spec_data = b'\x00' * img.sect_size

        start = lba * img.sect_size
        end = start + img.sect_size
        if end > len(other):
            other_data = other[start:] + b'\x00' * (end - len(other))
        else:
            other_data = other[start:end]

        if spec_data != other_data:
            mismatches += 1
            if first_mismatch is None:
                first_mismatch = lba
                print("FIRST MISMATCH at LBA=%d:" % lba)
                for off in range(min(32, img.sect_size)):
                    if spec_data[off] != other_data[off]:
                        print("  offset=%d: spec=0x%02X other=0x%02X" % (
                            off, spec_data[off], other_data[off]))

    total = img.total_lba
    print("\n結果: %d mismatch / %d LBA" % (mismatches, total))
    if mismatches == 0:
        print("✅ 完全一致")
    else:
        print("❌ 最初の不一致: LBA=%d" % first_mismatch)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    fdi_path = sys.argv[1]
    cmd = sys.argv[2]

    if not os.path.exists(fdi_path):
        print("エラー: ファイルが見つかりません: %s" % fdi_path, file=sys.stderr)
        sys.exit(1)

    img = FDIImage(fdi_path)

    if cmd == 'info':
        cmd_info(img)

    elif cmd == 'lba':
        if len(sys.argv) < 4:
            print("使い方: fdi_spec_parse.py <file> lba <N> [--count M]",
                  file=sys.stderr)
            sys.exit(1)
        start = int(sys.argv[3], 0)
        count = 1
        if len(sys.argv) >= 6 and sys.argv[4] == '--count':
            count = int(sys.argv[5], 0)
        cmd_lba(img, start, count)

    elif cmd == 'extract-raw':
        cmd_extract_raw(img)

    elif cmd == 'compare':
        if len(sys.argv) < 4:
            print("使い方: fdi_spec_parse.py <file> compare <other.bin>",
                  file=sys.stderr)
            sys.exit(1)
        cmd_compare(img, sys.argv[3])

    else:
        print("不明なコマンド: %s" % cmd, file=sys.stderr)
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
