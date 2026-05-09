#!/usr/bin/env python3
"""
d88_loop_verify.py — D88 ループバック三者比較ハーネス

根拠:
  - ground truth: d88_spec_parse.py (D88仕様書のみから実装)
  - lo0 (本実装): OS32 上の d88_loop.c ドライバ経由
  - fdd0 (v86_disk.c): OS32 上の V86 ディスク BIOSエミュレーション経由
  - analyze (参考): d88_analyze.py の出力

使い方:
  python3 d88_loop_verify.py --d88 <FILE> [--mode <all|spec|compare>]
                             [--os32-url <URL>] [--dump-dir <DIR>]
                             [--max-lba <N>] [--verbose]

モード:
  spec      ground truth のみ表示 (OS32 不要)
  compare   lo0 vs spec / fdd0 vs spec 比較 (OS32 + d88_loop 必要)
  all       全モード (デフォルト)

OS32 との通信:
  os32_server.py (デフォルト http://localhost:8032) を使用。
  /host/* はゲスト→ホスト共有フォルダ (/mnt/c/os32/)。
"""

import sys
import struct
import os
import argparse
import subprocess
import tempfile
import urllib.request
import urllib.parse
import json
import time

# ground truth パーサを同じディレクトリから import
_script_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _script_dir)
from d88_spec_parse import (
    D88SpecImage, D88ParseError, D88TrackAbsent, D88SectorNotFound,
    MEDIA_NAME, hexdump
)

# d88_analyze.py も同ディレクトリから import (比較対象として)
try:
    import importlib.util
    _analyze_spec = importlib.util.spec_from_file_location(
        "d88_analyze",
        os.path.join(_script_dir, "d88_analyze.py"))
    _d88_analyze = importlib.util.module_from_spec(_analyze_spec)
    _analyze_spec.loader.exec_module(_d88_analyze)
    D88Image = _d88_analyze.D88Image
    HAS_ANALYZE = True
except Exception as e:
    HAS_ANALYZE = False
    D88Image = None

# OS32 HostDrv 共有ディレクトリ (WSL パス)
HOST_SHARE_DIR = "/mnt/c/os32"


# ======================================================================
# OS32 HTTP API ユーティリティ
# ======================================================================

class OS32Client:
    """os32_server.py への HTTP コマンド送信クライアント"""

    def __init__(self, url="http://localhost:8032", timeout=30):
        self.base_url = url
        self.timeout  = timeout

    def cmd(self, command):
        """コマンドを送信して stdout 文字列を返す。エラー時は None。"""
        try:
            data = command.encode('utf-8')
            req = urllib.request.Request(
                self.base_url + "/cmd",
                data=data,
                method='POST',
                headers={'Content-Type': 'text/plain'}
            )
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.read().decode('utf-8', errors='replace')
        except Exception as e:
            return None

    def is_alive(self):
        """OS32 サーバが応答するか確認"""
        try:
            req = urllib.request.Request(
                self.base_url + "/status",
                method='GET'
            )
            with urllib.request.urlopen(req, timeout=5) as resp:
                return resp.status == 200
        except Exception:
            return False


# ======================================================================
# HostDrv 経由でセクタデータを取得するヘルパー
# ======================================================================

def _host_path_to_guest(host_path):
    """WSL パス /mnt/c/os32/foo → ゲスト側 /host/foo"""
    rel = os.path.relpath(host_path, HOST_SHARE_DIR)
    return "/host/" + rel.replace("\\", "/")


def fetch_lba_via_dd(client, dev_name, lba, bps, dump_dir):
    """
    OS32 の dd コマンドでセクタを読み出す。

    戻り値: bytes or None (エラー)
    """
    out_guest = "/host/verify_{}_lba{}.bin".format(dev_name, lba)
    out_host  = os.path.join(dump_dir, "verify_{}_lba{}.bin".format(dev_name, lba))

    cmd = "dd {} lba={} count=1 file={}".format(dev_name, lba, out_guest)
    result = client.cmd(cmd)
    if result is None:
        return None

    # ホスト側ファイルを読む
    time.sleep(0.2)  # HostDrv の書き込み反映待ち
    if not os.path.exists(out_host):
        return None

    with open(out_host, 'rb') as f:
        data = f.read()

    os.remove(out_host)  # 次回と干渉しないよう削除
    return data if len(data) == bps else None


# ======================================================================
# d88_analyze.py からセクタを取得するヘルパー
# ======================================================================

def fetch_lba_via_analyze(analyze_img, lba, bps, cyls, heads, spt):
    """
    d88_analyze.py の sector_map を使って LBA N のデータを返す。

    d88_analyze.py は (C, H, R) → データ の map を持つ。
    LBA → (C, H, R) は d88_spec_parse.py と同じ計算で行う。

    戻り値: bytes or None
    """
    if not HAS_ANALYZE or analyze_img is None:
        return None

    # LBA → CHS (spec と同じ計算)
    sec0  = lba % spt
    track = lba // spt
    head  = track % heads
    cyl   = track // heads
    r     = sec0 + 1  # 1 始まり

    key = (cyl, head, r)
    if key not in analyze_img.sector_map:
        return None

    (data_off, data_size) = analyze_img.sector_map[key]
    raw = analyze_img.data[data_off:data_off + data_size]
    if len(raw) < bps:
        raw = raw + b'\x00' * (bps - len(raw))
    return raw[:bps]


# ======================================================================
# 比較ロジック
# ======================================================================

class MismatchRecord:
    def __init__(self, lba, cyl, head, sect_r, source_a, source_b,
                 data_a, data_b):
        self.lba     = lba
        self.cyl     = cyl
        self.head    = head
        self.sect_r  = sect_r
        self.source_a = source_a
        self.source_b = source_b
        self.data_a  = data_a
        self.data_b  = data_b

    def __str__(self):
        first_diff = next(
            (i for i, (a, b) in enumerate(zip(self.data_a, self.data_b))
             if a != b), -1)
        return (
            "LBA={} (C={} H={} R={}) {} vs {}: "
            "first_diff=offset+{}\n"
            "  {}[0..15]: {}\n"
            "  {}[0..15]: {}".format(
                self.lba, self.cyl, self.head, self.sect_r,
                self.source_a, self.source_b,
                first_diff,
                self.source_a,
                ' '.join('{:02X}'.format(b) for b in self.data_a[:16]),
                self.source_b,
                ' '.join('{:02X}'.format(b) for b in self.data_b[:16]),
            )
        )


def compare_all_lba(img, client, dump_dir, max_lba, verbose, os32_url):
    """全 LBA を走査して不一致を記録する"""
    total_lba = img.total_lba
    if max_lba is not None and max_lba < total_lba:
        total_lba = max_lba

    bps   = img.lba_bps
    spt   = img.lba_spt
    heads = img.heads
    cyls  = img.cyls

    # analyze.py イメージ (参考比較用)
    if HAS_ANALYZE:
        try:
            # d88_analyze.py は引数でファイルパスを受け取らないのでダミー経由
            analyze_img = D88Image(args_d88_path)  # グローバルから参照
        except Exception:
            analyze_img = None
    else:
        analyze_img = None

    mismatches_lo0  = []   # lo0 vs spec
    mismatches_fdd0 = []   # fdd0 vs spec
    mismatches_ana  = []   # analyze vs spec

    use_lo0  = (client is not None) and client.is_alive()
    use_fdd0 = use_lo0  # 同じ OS32 サーバを使う

    print("=== 比較開始: total_lba={} bps={} ===".format(total_lba, bps))
    if use_lo0:
        print("  OS32 接続: {} (lo0, fdd0)".format(os32_url))
    else:
        print("  OS32 未接続 — spec vs analyze のみ")

    for lba in range(total_lba):
        (cyl, head, sect_r) = img.lba_to_chs(lba)

        # --- ground truth 取得 ---
        try:
            spec_data = img.read_lba(lba)
        except D88TrackAbsent:
            if verbose:
                print("LBA={} SKIP (不在トラック)".format(lba))
            continue
        except (D88ParseError, D88SectorNotFound) as e:
            print("LBA={} SPEC_ERROR: {}".format(lba, e))
            continue

        # --- lo0 取得 ---
        if use_lo0:
            lo0_data = fetch_lba_via_dd(client, "lo0", lba, bps, dump_dir)
            if lo0_data is None:
                print("LBA={} lo0: 取得失敗".format(lba))
            elif lo0_data != spec_data:
                mismatches_lo0.append(
                    MismatchRecord(lba, cyl, head, sect_r,
                                   "spec", "lo0", spec_data, lo0_data))
                if verbose:
                    print("MISMATCH lo0  LBA={}".format(lba))

        # --- fdd0 取得 ---
        if use_fdd0:
            fdd0_data = fetch_lba_via_dd(client, "fdd0", lba, bps, dump_dir)
            if fdd0_data is None:
                print("LBA={} fdd0: 取得失敗".format(lba))
            elif fdd0_data != spec_data:
                mismatches_fdd0.append(
                    MismatchRecord(lba, cyl, head, sect_r,
                                   "spec", "fdd0", spec_data, fdd0_data))
                if verbose:
                    print("MISMATCH fdd0 LBA={}".format(lba))

        # --- analyze 取得 ---
        if HAS_ANALYZE and analyze_img is not None:
            ana_data = fetch_lba_via_analyze(
                analyze_img, lba, bps, cyls, heads, spt)
            if ana_data is not None and ana_data != spec_data:
                mismatches_ana.append(
                    MismatchRecord(lba, cyl, head, sect_r,
                                   "spec", "analyze", spec_data, ana_data))
                if verbose:
                    print("MISMATCH analyze LBA={}".format(lba))

        if lba % 100 == 0:
            sys.stdout.write("\r  進捗: {}/{} LBA".format(lba + 1, total_lba))
            sys.stdout.flush()

    print("\r  完了: {} LBA 走査".format(total_lba))

    # --- 結果レポート ---
    print()
    print("=== 比較結果 ===")
    print("lo0   vs spec : {} mismatch / {} LBA".format(
        len(mismatches_lo0), total_lba))
    print("fdd0  vs spec : {} mismatch / {} LBA".format(
        len(mismatches_fdd0), total_lba))
    print("analyze vs spec: {} mismatch / {} LBA".format(
        len(mismatches_ana), total_lba))

    if mismatches_lo0:
        print()
        print("--- lo0 最初の不一致 (最大 5 件) ---")
        for m in mismatches_lo0[:5]:
            print(str(m))

    if mismatches_fdd0:
        print()
        print("--- fdd0 最初の不一致 (最大 10 件) ---")
        for m in mismatches_fdd0[:10]:
            print(str(m))

    if mismatches_ana:
        print()
        print("--- analyze 最初の不一致 (最大 5 件) ---")
        for m in mismatches_ana[:5]:
            print(str(m))

    return len(mismatches_lo0), len(mismatches_fdd0), len(mismatches_ana)


# ======================================================================
# spec のみモード (OS32 不要)
# ======================================================================

def mode_spec(img, max_lba, verbose):
    """ground truth (spec) の全 LBA を列挙して表示"""
    total_lba = img.total_lba
    if max_lba is not None and max_lba < total_lba:
        total_lba = max_lba

    ok = 0
    absent = 0
    errors = 0
    for lba in range(total_lba):
        try:
            data = img.read_lba(lba)
            ok += 1
            if verbose:
                (cyl, head, sect_r) = img.lba_to_chs(lba)
                print("LBA={:4d} C={:3d} H={} R={:3d} first4={}".format(
                    lba, cyl, head, sect_r,
                    data[:4].hex()))
        except D88TrackAbsent:
            absent += 1
            if verbose:
                print("LBA={:4d} ABSENT".format(lba))
        except (D88ParseError, D88SectorNotFound) as e:
            errors += 1
            print("LBA={:4d} ERROR: {}".format(lba, e))

    print()
    print("=== spec モード結果 ===")
    print("  OK    : {}".format(ok))
    print("  ABSENT: {}".format(absent))
    print("  ERROR : {}".format(errors))
    print("  合計  : {}".format(total_lba))


# ======================================================================
# エントリポイント
# ======================================================================

# グローバル変数 (compare_all_lba から参照)
args_d88_path = None


def main():
    global args_d88_path

    parser = argparse.ArgumentParser(
        description="D88 ループバック三者比較ハーネス")
    parser.add_argument('--d88',       required=True,
                        help="D88 ファイルパス")
    parser.add_argument('--mode',      default='all',
                        choices=['all', 'spec', 'compare'],
                        help="実行モード (デフォルト: all)")
    parser.add_argument('--os32-url',  default='http://localhost:8032',
                        help="os32_server.py の URL")
    parser.add_argument('--dump-dir',  default='/mnt/c/os32',
                        help="HostDrv 共有ディレクトリ (WSL パス)")
    parser.add_argument('--max-lba',   type=int, default=None,
                        help="最大 LBA 数 (デバッグ用)")
    parser.add_argument('--verbose',   action='store_true',
                        help="詳細ログ出力")
    args = parser.parse_args()

    args_d88_path = args.d88

    if not os.path.exists(args.d88):
        sys.stderr.write("エラー: ファイルが見つからない: {}\n".format(args.d88))
        sys.exit(1)

    try:
        img = D88SpecImage(args.d88)
    except D88ParseError as e:
        sys.stderr.write("D88ParseError: {}\n".format(e))
        sys.exit(1)

    print("D88: {} | media={} | total_lba={} | bps={}".format(
        args.d88,
        MEDIA_NAME.get(img.media, "0x{:02X}".format(img.media)),
        img.total_lba,
        img.lba_bps))
    print()

    if args.mode in ('all', 'spec'):
        print("--- [spec モード] ---")
        mode_spec(img, args.max_lba, args.verbose)
        print()

    if args.mode in ('all', 'compare'):
        print("--- [compare モード] ---")
        client = OS32Client(args.os32_url)
        lo0_mm, fdd0_mm, ana_mm = compare_all_lba(
            img, client, args.dump_dir,
            args.max_lba, args.verbose, args.os32_url)
        # 終了コード: lo0 が完全一致なら 0
        sys.exit(0 if lo0_mm == 0 else 2)


if __name__ == '__main__':
    main()
