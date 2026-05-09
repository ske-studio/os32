#!/usr/bin/env python3
"""d88_spec_parse.py — D88仕様書準拠パーサ (ground truth)

本スクリプトは docs/D88_FORMAT_SPEC.md のみを根拠に書き起こした
D88 パーサであり、d88_analyze.py / v86_disk.c とは独立している。
v86_disk.c の検証における ground truth として使用する。

使い方:
  python3 d88_spec_parse.py <D88ファイル> <コマンド> [オプション]

コマンド:
  info                        ヘッダ情報を表示
  tracks                      全トラックのオフセット・SPT・N値一覧
  sectors                     全セクタの C/H/R/N/data_len 一覧
  lba <N> [--count M]         LBA N から M セクタ分のデータをバイナリ出力
  sector <C> <H> <R>          特定 CHS セクタのデータをバイナリ出力
  extract-raw                 D88 → RAW 変換 (round-trip 検証用)
  compare <other.bin>         LBA全走査で他ツール出力と差分比較

設計根拠:
  §3  ヘッダ (0x2B0 bytes): media(0x1B), disk_size(0x1C), track_table(0x20)
  §5  セクタヘッダ (16 bytes): C/H/R/N/SPT/density/deleted/status/rsv/data_len
  §6  N → セクタサイズ: 128 << N
  §9  メディア別パラメータ: 2D/2DD/2HD/1D/1DD
  §12 セクタ検索: C/H/R 照合, data_len フィールド優先
"""

import sys
import struct
import os

D88_HEADER_SIZE = 0x2B0
MAX_TRACKS = 164
SECT_HDR_SIZE = 16

# メディア種別 → (cyls, heads) の対応表 (§9)
MEDIA_GEOM = {
    0x00: (40, 2, '2D'),
    0x10: (80, 2, '2DD'),
    0x20: (77, 2, '2HD'),
    0x30: (40, 1, '1D'),
    0x40: (80, 1, '1DD'),
}


class D88SpecImage:
    """D88仕様書準拠のパーサ"""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()

        if len(self.data) < D88_HEADER_SIZE:
            raise ValueError(f"ファイルサイズ不足: {len(self.data)} < {D88_HEADER_SIZE}")

        # §3 ヘッダ解析
        self.name = self.data[0:17].split(b'\x00')[0].decode('ascii', errors='replace')
        self.write_protect = self.data[0x1A]
        self.media = self.data[0x1B]
        self.disk_size = struct.unpack_from('<I', self.data, 0x1C)[0]

        # §3 トラックオフセットテーブル
        self.track_offsets = []
        for i in range(MAX_TRACKS):
            off = struct.unpack_from('<I', self.data, 0x20 + i * 4)[0]
            self.track_offsets.append(off)

        # media → cyls/heads (§9)
        if self.media not in MEDIA_GEOM:
            raise ValueError(f"未知のメディア種別: 0x{self.media:02X}")
        self.cyls, self.heads, self.media_name = MEDIA_GEOM[self.media]

        # トラック0のセクタヘッダから SPT/N を取得
        # PC-88 2HD (N=1, SPT=26) と PC-98 2HD (N=3, SPT=8) を自動判別
        t0_off = self.track_offsets[0]
        if t0_off == 0 or t0_off >= len(self.data):
            raise ValueError("トラック0が存在しない")

        hdr = self.data[t0_off:t0_off + SECT_HDR_SIZE]
        if len(hdr) < SECT_HDR_SIZE:
            raise ValueError("トラック0のセクタヘッダが不完全")

        self.n_val = hdr[3]
        self.spt = struct.unpack_from('<H', hdr, 4)[0]
        self.sect_size = 128 << self.n_val
        self.total_lba = self.cyls * self.heads * self.spt

        # 全セクタリスト構築 (表示用: track_idx 付きで重複 C/H/R を保持)
        self.sector_list = []
        self._build_sector_list()

    def _build_sector_list(self):
        """§4, §5 に従い全トラックのセクタヘッダを走査

        注意: コピープロテクション付きイメージでは異なる物理トラックに
        同じ C/H/R を持つセクタが存在する。dict ではなく list で保持し、
        track_idx を付与して物理トラックを区別する。"""
        for track_idx in range(MAX_TRACKS):
            trk_off = self.track_offsets[track_idx]
            if trk_off == 0:
                continue

            pos = trk_off
            if pos + SECT_HDR_SIZE > len(self.data):
                continue

            # 先頭セクタから SPT を取得
            first_spt = struct.unpack_from('<H', self.data, pos + 4)[0]
            if first_spt == 0 or first_spt > 64:
                continue

            for _ in range(first_spt):
                if pos + SECT_HDR_SIZE > len(self.data):
                    break

                c = self.data[pos]
                h = self.data[pos + 1]
                r = self.data[pos + 2]
                n = self.data[pos + 3]
                spt_field = struct.unpack_from('<H', self.data, pos + 4)[0]
                density = self.data[pos + 6]
                deleted = self.data[pos + 7]
                status = self.data[pos + 8]
                data_len = struct.unpack_from('<H', self.data, pos + 14)[0]

                data_offset = pos + SECT_HDR_SIZE

                self.sector_list.append({
                    'track_idx': track_idx,
                    'c': c, 'h': h, 'r': r,
                    'data_offset': data_offset,
                    'data_len': data_len,
                    'n': n,
                    'spt': spt_field,
                    'density': density,
                    'deleted': deleted,
                    'status': status,
                    'hdr_offset': pos,
                })

                pos += SECT_HDR_SIZE + data_len

    def lba_to_chs(self, lba):
        """LBA → (C, H, R_1based) 変換"""
        track = lba // self.spt
        cyl = track // self.heads
        head = track % self.heads
        r = (lba % self.spt) + 1
        return (cyl, head, r)

    def _read_from_track(self, track_idx, target_c, target_h, target_r):
        """物理トラック内でセクタを検索してデータを返す (d88_loop.c と同一アルゴリズム)

        C/H/R 厳密照合: 実機 FDC の READ と同じく全フィールドを照合する。
        コピープロテクションで H 値が不正なセクタは「未発見」として None を返す。"""
        if track_idx >= MAX_TRACKS:
            return None
        trk_off = self.track_offsets[track_idx]
        if trk_off == 0 or trk_off >= len(self.data):
            return None

        pos = trk_off
        if pos + SECT_HDR_SIZE > len(self.data):
            return None

        track_spt = struct.unpack_from('<H', self.data, pos + 4)[0]
        if track_spt == 0 or track_spt > 64:
            return None

        for _ in range(track_spt):
            if pos + SECT_HDR_SIZE > len(self.data):
                return None
            c = self.data[pos]
            h = self.data[pos + 1]
            r = self.data[pos + 2]
            data_len = struct.unpack_from('<H', self.data, pos + 14)[0]
            if data_len == 0:
                pos += SECT_HDR_SIZE
                continue
            if c == target_c and h == target_h and r == target_r:
                data_offset = pos + SECT_HDR_SIZE
                raw = self.data[data_offset:data_offset + data_len]
                if len(raw) < self.sect_size:
                    raw += b'\x00' * (self.sect_size - len(raw))
                elif len(raw) > self.sect_size:
                    raw = raw[:self.sect_size]
                return raw
            pos += SECT_HDR_SIZE + data_len

        return None

    def read_lba(self, lba):
        """LBA で指定されたセクタのデータを返す (bytes)

        d88_loop.c と同一のアルゴリズム: LBA → track_idx → 物理トラック走査。
        sector_map dict は使わない (コピープロテクション対応)。"""
        c, h, r = self.lba_to_chs(lba)
        track_idx = c * self.heads + h
        return self._read_from_track(track_idx, c, h, r)

    def read_sector_chs(self, c, h, r):
        """CHS で指定されたセクタのデータを返す (C/H/R 厳密照合)"""
        track_idx = c * self.heads + h
        return self._read_from_track(track_idx, c, h, r)

    def extract_raw(self):
        """D88 → RAW 変換: 全 LBA を順番に連結"""
        result = bytearray()
        for lba in range(self.total_lba):
            data = self.read_lba(lba)
            if data is None:
                result += b'\x00' * self.sect_size
            else:
                result += data
        return bytes(result)


def cmd_info(img):
    print(f"ディスク名    : {img.name}")
    print(f"メディア種別  : 0x{img.media:02X} ({img.media_name})")
    print(f"ディスクサイズ: {img.disk_size} bytes ({img.disk_size // 1024} KB)")
    print(f"ライトプロテクト: {'あり' if img.write_protect == 0x10 else 'なし'}")
    print(f"シリンダ数    : {img.cyls}")
    print(f"ヘッド数      : {img.heads}")
    print(f"SPT           : {img.spt}")
    print(f"N値           : {img.n_val} (= {img.sect_size} B/sect)")
    print(f"総LBA数       : {img.total_lba}")
    print(f"データ容量    : {img.total_lba * img.sect_size} bytes "
          f"({img.total_lba * img.sect_size // 1024} KB)")
    print(f"セクタマップ  : {len(img.sector_list)} セクタ")

    # 有効トラック数
    active = sum(1 for off in img.track_offsets if off != 0)
    print(f"有効トラック  : {active}")


def cmd_tracks(img):
    print(f"{'Idx':>4} {'Cyl':>4} {'Head':>4} {'Offset':>10} "
          f"{'SPT':>4} {'N':>2} {'BPS':>5}")
    print("-" * 44)

    for track_idx in range(MAX_TRACKS):
        off = img.track_offsets[track_idx]
        if off == 0:
            continue

        cyl = track_idx // img.heads
        head = track_idx % img.heads

        if off + SECT_HDR_SIZE <= len(img.data):
            n = img.data[off + 3]
            spt = struct.unpack_from('<H', img.data, off + 4)[0]
            bps = 128 << n
        else:
            n = spt = bps = 0

        print(f"{track_idx:4d} {cyl:4d} {head:4d} 0x{off:08X} "
              f"{spt:4d} {n:2d} {bps:5d}")


def cmd_sectors(img):
    print(f"{'Trk':>4} {'C':>3} {'H':>2} {'R':>3} {'N':>2} {'BPS':>5} "
          f"{'DataLen':>7} {'Status':>6} {'Del':>3} {'Offset':>10}")
    print("-" * 62)

    for info in img.sector_list:
        bps = 128 << info['n']
        print(f"{info['track_idx']:4d} {info['c']:3d} {info['h']:2d} {info['r']:3d} "
              f"{info['n']:2d} {bps:5d} "
              f"{info['data_len']:7d} 0x{info['status']:02X}   "
              f"{'D' if info['deleted'] else ' ':>3s} "
              f"0x{info['hdr_offset']:08X}")


def cmd_lba(img, start_lba, count):
    for i in range(count):
        lba = start_lba + i
        if lba >= img.total_lba:
            print(f"LBA {lba} は範囲外 (total={img.total_lba})",
                  file=sys.stderr)
            break
        data = img.read_lba(lba)
        if data is None:
            print(f"LBA {lba} データなし", file=sys.stderr)
            sys.stdout.buffer.write(b'\x00' * img.sect_size)
        else:
            sys.stdout.buffer.write(data)


def cmd_sector(img, c, h, r):
    data = img.read_sector_chs(c, h, r)
    if data is None:
        print(f"セクタ C={c} H={h} R={r} が見つかりません",
              file=sys.stderr)
        sys.exit(1)
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
            c, h, r = img.lba_to_chs(lba)
            if first_mismatch is None:
                first_mismatch = (lba, c, h, r)

                # 最初の不一致箇所の詳細
                print(f"FIRST MISMATCH at LBA={lba} C={c} H={h} R={r}:")
                for off in range(min(32, img.sect_size)):
                    if spec_data[off] != other_data[off]:
                        print(f"  offset={off}: spec=0x{spec_data[off]:02X} "
                              f"other=0x{other_data[off]:02X}")

    total = img.total_lba
    print(f"\n結果: {mismatches} mismatch / {total} LBA")
    if mismatches == 0:
        print("✅ 完全一致")
    else:
        lba, c, h, r = first_mismatch
        print(f"❌ 最初の不一致: LBA={lba} C={c} H={h} R={r}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    d88_path = sys.argv[1]
    cmd = sys.argv[2]

    if not os.path.exists(d88_path):
        print(f"エラー: ファイルが見つかりません: {d88_path}", file=sys.stderr)
        sys.exit(1)

    img = D88SpecImage(d88_path)

    if cmd == 'info':
        cmd_info(img)

    elif cmd == 'tracks':
        cmd_tracks(img)

    elif cmd == 'sectors':
        cmd_sectors(img)

    elif cmd == 'lba':
        if len(sys.argv) < 4:
            print("使い方: d88_spec_parse.py <file> lba <N> [--count M]",
                  file=sys.stderr)
            sys.exit(1)
        start = int(sys.argv[3], 0)
        count = 1
        if len(sys.argv) >= 6 and sys.argv[4] == '--count':
            count = int(sys.argv[5], 0)
        cmd_lba(img, start, count)

    elif cmd == 'sector':
        if len(sys.argv) < 6:
            print("使い方: d88_spec_parse.py <file> sector <C> <H> <R>",
                  file=sys.stderr)
            sys.exit(1)
        c = int(sys.argv[3], 0)
        h = int(sys.argv[4], 0)
        r = int(sys.argv[5], 0)
        cmd_sector(img, c, h, r)

    elif cmd == 'extract-raw':
        cmd_extract_raw(img)

    elif cmd == 'compare':
        if len(sys.argv) < 4:
            print("使い方: d88_spec_parse.py <file> compare <other.bin>",
                  file=sys.stderr)
            sys.exit(1)
        cmd_compare(img, sys.argv[3])

    else:
        print(f"不明なコマンド: {cmd}", file=sys.stderr)
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
