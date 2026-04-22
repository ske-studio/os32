#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pyxres2os32.py — Pyxel .pyxres → OS32 .os32res 変換ツール

Pyxel 2.x の .pyxres (ZIP + TOML) を OS32 ネイティブの
フラットバイナリ .os32res に変換する。

使用法:
    python3 pyxres2os32.py input.pyxres [-o output.os32res] [-v]

.os32res フォーマット仕様:
    docs/tasks/libpyxel/02_VRAM_PLANAR.md §5 を参照。
"""

import sys
import os
import struct
import zipfile
import argparse

# Python 3.11+ は tomllib (stdlib), それ以前は tomli (pip)
try:
    import tomllib
except ImportError:
    try:
        import tomli as tomllib
    except ImportError:
        print("エラー: Python 3.11+ の tomllib または pip install tomli が必要です",
              file=sys.stderr)
        sys.exit(1)

# ========================================================================
#  定数
# ========================================================================

MAGIC = b"PX32"
FORMAT_VERSION = 1
HEADER_SIZE = 32  # 0x20

# イメージバンクの固定サイズ
IMG_WIDTH = 256
IMG_HEIGHT = 256
IMG_BPL = IMG_WIDTH // 8  # 32 bytes/line
IMG_PLANE_SIZE = IMG_BPL * IMG_HEIGHT  # 8192 bytes/plane
IMG_BANK_SIZE = IMG_PLANE_SIZE * 4  # 32768 bytes/bank

# 最大エントリ数
MAX_IMAGES = 3
MAX_TILEMAPS = 8
MAX_SOUNDS = 64
MAX_MUSICS = 8

# サウンドの最大ノート数 (メモリ制約)
MAX_NOTES = 256


# ========================================================================
#  末尾値圧縮の展開
# ========================================================================

def expand_row(row, target_len):
    """末尾値圧縮された配列を target_len に展開する。

    Pyxel は TOML の各行で末尾の繰り返し値を省略する。
    例: [0, 0, 5, 5, 5, 5] → [0, 0, 5] として保存。
    """
    if not row:
        return [0] * target_len
    if len(row) >= target_len:
        return list(row[:target_len])
    fill_val = row[-1]
    return list(row) + [fill_val] * (target_len - len(row))


def expand_2d(data, width, height):
    """2D配列を完全な width×height に展開する。

    行方向 (末尾行の省略) と列方向 (末尾値の省略) の両方を処理する。
    """
    result = []
    last_row = [0]
    for i in range(height):
        if i < len(data):
            row = expand_row(data[i], width)
            last_row = data[i]
        else:
            row = expand_row(last_row, width)
        result.append(row)
    return result


# ========================================================================
#  イメージバンク変換 (パックトピクセル → プレーナー)
# ========================================================================

def is_image_empty(pixels):
    """イメージバンクが全て0 (空) かどうかを判定する。"""
    for row in pixels:
        for px in row:
            if px != 0:
                return False
    return True


def pack_to_planar(pixels, w, h):
    """パックトピクセル (0-15) → 4プレーン × (w/8) × h バイト。

    プレーン順: B(bit0), R(bit1), G(bit2), I(bit3)
    ビット順: MSBファースト (x=0 が bit7)
    """
    bpl = w // 8
    planes = [bytearray(bpl * h) for _ in range(4)]

    for y in range(h):
        for xb in range(bpl):
            b_val = [0, 0, 0, 0]
            for bit in range(8):
                x = xb * 8 + bit
                color = pixels[y][x] & 0x0F
                mask = 0x80 >> bit
                for p in range(4):
                    if color & (1 << p):
                        b_val[p] |= mask
            offset = y * bpl + xb
            for p in range(4):
                planes[p][offset] = b_val[p]

    return planes


def convert_image(img_toml, verbose=False):
    """イメージバンクTOMLオブジェクト → プレーナーバイナリ。

    戻り値: bytes (IMG_BANK_SIZE = 32768 バイト) または None (空バンク)
    """
    w = img_toml.get("width", IMG_WIDTH)
    h = img_toml.get("height", IMG_HEIGHT)
    data = img_toml.get("data", [])

    pixels = expand_2d(data, w, h)

    if is_image_empty(pixels):
        return None

    # 16色を超えるインデックスの検出
    over_range = 0
    for row in pixels:
        for px in row:
            if px > 15:
                over_range += 1

    if over_range > 0 and verbose:
        print("  警告: %d ピクセルが 16色範囲外 (0x0F でクランプ)" % over_range)

    # プレーナー変換 (256×256 固定)
    full_pixels = []
    for y in range(IMG_HEIGHT):
        row = [0] * IMG_WIDTH
        for x in range(min(w, IMG_WIDTH)):
            if y < h:
                row[x] = pixels[y][x]
        full_pixels.append(row)

    planes = pack_to_planar(full_pixels, IMG_WIDTH, IMG_HEIGHT)

    # B, R, G, I 順で連結
    result = bytearray()
    for p in range(4):
        result += planes[p]

    return bytes(result)


# ========================================================================
#  タイルマップ変換
# ========================================================================

def is_tilemap_empty(tm_toml):
    """タイルマップが空かどうかを判定する。"""
    data = tm_toml.get("data", [])
    if not data:
        return True
    for row in data:
        for val in row:
            if val != 0:
                return False
    return True


def convert_tilemap(tm_toml, verbose=False):
    """タイルマップTOMLオブジェクト → バイナリ。

    TOML の data はインターリーブ形式: [tx0, ty0, tx1, ty1, ...]
    .os32res では (tx_u8, ty_u8) ペアとして格納。

    戻り値: bytes またはNone (空マップ)
    """
    if is_tilemap_empty(tm_toml):
        return None

    width = tm_toml.get("width", 256)
    height = tm_toml.get("height", 256)
    imgsrc = tm_toml.get("imgsrc", 0)
    data = tm_toml.get("data", [])

    # インターリーブ形式なので行の長さは width * 2
    expanded = expand_2d(data, width * 2, height)

    buf = bytearray()
    # ヘッダ: imgsrc(1) + padding(1) + width(2) + height(2)
    buf += struct.pack("<BBHH", imgsrc & 0xFF, 0, width, height)

    # タイルデータ: (tx, ty) ペア
    for row in expanded:
        for i in range(0, width * 2, 2):
            tx = row[i] & 0xFF
            ty = row[i + 1] & 0xFF if i + 1 < len(row) else 0
            buf += struct.pack("BB", tx, ty)

    if verbose:
        tile_count = width * height
        non_zero = 0
        for row in expanded:
            for i in range(0, width * 2, 2):
                if row[i] != 0 or (i + 1 < len(row) and row[i + 1] != 0):
                    non_zero += 1
        print("  タイルマップ: %dx%d, バンク=%d, 非空タイル=%d/%d" % (
            width, height, imgsrc, non_zero, tile_count))

    return bytes(buf)


# ========================================================================
#  サウンド変換
# ========================================================================

def is_sound_empty(snd_toml):
    """サウンドが空かどうかを判定する。"""
    notes = snd_toml.get("notes", [])
    return len(notes) == 0


def convert_sound(snd_toml, verbose=False):
    """サウンドTOMLオブジェクト → バイナリ。

    戻り値: bytes またはNone (空サウンド)
    """
    notes = snd_toml.get("notes", [])
    if not notes:
        return None

    speed = snd_toml.get("speed", 30)
    note_count = min(len(notes), MAX_NOTES)

    # 末尾値圧縮の展開 (tones/volumes/effects は notes より短い場合がある)
    tones = expand_row(snd_toml.get("tones", [0]), note_count)
    volumes = expand_row(snd_toml.get("volumes", [7]), note_count)
    effects = expand_row(snd_toml.get("effects", [0]), note_count)

    buf = bytearray()
    # ヘッダ: speed(2) + note_count(2)
    buf += struct.pack("<HH", speed, note_count)

    # notes (i8: -1=休符)
    for i in range(note_count):
        n = notes[i]
        if n < -1:
            n = -1
        buf += struct.pack("b", n)

    # tones (u8)
    for i in range(note_count):
        buf += struct.pack("B", tones[i] & 0xFF)

    # volumes (u8)
    for i in range(note_count):
        buf += struct.pack("B", volumes[i] & 0xFF)

    # effects (u8)
    for i in range(note_count):
        buf += struct.pack("B", effects[i] & 0xFF)

    return bytes(buf)


# ========================================================================
#  ミュージック変換
# ========================================================================

def is_music_empty(mus_toml):
    """ミュージックが空かどうかを判定する。"""
    seqs = mus_toml.get("seqs", [])
    if not seqs:
        return True
    for ch_seq in seqs:
        if ch_seq:
            return False
    return True


def convert_music(mus_toml, verbose=False):
    """ミュージックTOMLオブジェクト → バイナリ。

    戻り値: bytes またはNone (空ミュージック)
    """
    seqs = mus_toml.get("seqs", [])
    if not seqs:
        return None

    # 末尾の空チャンネルを除去
    while seqs and not seqs[-1]:
        seqs = seqs[:-1]

    if not seqs:
        return None

    channel_count = len(seqs)

    buf = bytearray()
    buf += struct.pack("<H", channel_count)

    for ch_seq in seqs:
        seq_count = len(ch_seq) if ch_seq else 0
        buf += struct.pack("<H", seq_count)
        for snd_idx in (ch_seq or []):
            buf += struct.pack("<H", snd_idx & 0xFFFF)

    return bytes(buf)


# ========================================================================
#  .os32res ビルダー
# ========================================================================

def build_os32res(toml_data, verbose=False):
    """TOMLデータ全体を .os32res バイナリに変換する。

    空セクションはスキップし、実エントリのみを出力する。
    """
    # --- イメージバンク ---
    images = toml_data.get("images", [])
    img_bins = []
    for i, img in enumerate(images[:MAX_IMAGES]):
        if verbose:
            print("イメージバンク %d の変換中..." % i)
        result = convert_image(img, verbose)
        if result is not None:
            img_bins.append(result)
        else:
            if verbose:
                print("  スキップ (空)")

    # --- タイルマップ ---
    tilemaps = toml_data.get("tilemaps", [])
    tm_bins = []
    for i, tm in enumerate(tilemaps[:MAX_TILEMAPS]):
        result = convert_tilemap(tm, verbose)
        if result is not None:
            tm_bins.append(result)

    if verbose and tm_bins:
        print("タイルマップ: %d 個変換" % len(tm_bins))

    # --- サウンド ---
    sounds = toml_data.get("sounds", [])
    snd_bins = []
    for i, snd in enumerate(sounds[:MAX_SOUNDS]):
        result = convert_sound(snd, verbose)
        if result is not None:
            snd_bins.append(result)

    if verbose and snd_bins:
        print("サウンド: %d 個変換" % len(snd_bins))

    # --- ミュージック ---
    musics = toml_data.get("musics", [])
    mus_bins = []
    for i, mus in enumerate(musics[:MAX_MUSICS]):
        result = convert_music(mus, verbose)
        if result is not None:
            mus_bins.append(result)

    if verbose and mus_bins:
        print("ミュージック: %d 個変換" % len(mus_bins))

    # --- セクションデータ構築 ---
    img_section = bytearray()
    for b in img_bins:
        img_section += b

    tm_section = bytearray()
    for b in tm_bins:
        tm_section += b

    snd_section = bytearray()
    for b in snd_bins:
        snd_section += b

    mus_section = bytearray()
    for b in mus_bins:
        mus_section += b

    # --- オフセット計算 ---
    img_offset = HEADER_SIZE
    tm_offset = img_offset + len(img_section)
    snd_offset = tm_offset + len(tm_section)
    mus_offset = snd_offset + len(snd_section)

    # --- ヘッダ構築 ---
    header = bytearray()
    header += MAGIC                                         # 0x00: マジック
    header += struct.pack("<I", FORMAT_VERSION)              # 0x04: バージョン
    header += struct.pack("<H", len(img_bins))               # 0x08: イメージ数
    header += struct.pack("<H", len(tm_bins))                # 0x0A: タイルマップ数
    header += struct.pack("<H", len(snd_bins))               # 0x0C: サウンド数
    header += struct.pack("<H", len(mus_bins))               # 0x0E: ミュージック数
    header += struct.pack("<I", img_offset)                  # 0x10: イメージオフセット
    header += struct.pack("<I", tm_offset)                   # 0x14: タイルマップオフセット
    header += struct.pack("<I", snd_offset)                  # 0x18: サウンドオフセット
    header += struct.pack("<I", mus_offset)                  # 0x1C: ミュージックオフセット

    assert len(header) == HEADER_SIZE

    # --- 結合 ---
    result = bytes(header) + bytes(img_section) + bytes(tm_section) + \
             bytes(snd_section) + bytes(mus_section)

    if verbose:
        total = len(result)
        print("--- 出力統計 ---")
        print("  イメージバンク: %d 個 (%d bytes)" % (len(img_bins), len(img_section)))
        print("  タイルマップ:   %d 個 (%d bytes)" % (len(tm_bins), len(tm_section)))
        print("  サウンド:       %d 個 (%d bytes)" % (len(snd_bins), len(snd_section)))
        print("  ミュージック:   %d 個 (%d bytes)" % (len(mus_bins), len(mus_section)))
        print("  合計: %d bytes (%.1f KB)" % (total, total / 1024.0))

    return result


# ========================================================================
#  .pyxres ローダー
# ========================================================================

def load_pyxres(path):
    """Pyxel .pyxres ファイルを読み込んで TOML dict を返す。

    .pyxres は ZIP アーカイブで、内部に pyxel_resource.toml を含む。
    レガシー形式 (ディレクトリ構造) も検出する。
    """
    if not os.path.exists(path):
        print("エラー: ファイルが見つかりません: %s" % path, file=sys.stderr)
        sys.exit(1)

    if not zipfile.is_zipfile(path):
        print("エラー: ZIPファイルではありません: %s" % path, file=sys.stderr)
        sys.exit(1)

    with zipfile.ZipFile(path, "r") as zf:
        names = zf.namelist()

        # pyxel_resource.toml を探す
        toml_name = None
        for name in names:
            if name.endswith("pyxel_resource.toml"):
                toml_name = name
                break

        if toml_name is None:
            print("エラー: pyxel_resource.toml が見つかりません: %s" % path,
                  file=sys.stderr)
            print("  ZIP内のファイル: %s" % names, file=sys.stderr)
            sys.exit(1)

        toml_bytes = zf.read(toml_name)

    data = tomllib.loads(toml_bytes.decode("utf-8"))
    return data


# ========================================================================
#  メイン
# ========================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Pyxel .pyxres → OS32 .os32res 変換ツール")
    parser.add_argument("input", help="入力 .pyxres ファイルパス")
    parser.add_argument("-o", "--output",
                        help="出力 .os32res ファイルパス (デフォルト: 入力ファイル名.os32res)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="変換統計を表示")
    args = parser.parse_args()

    # 出力パスのデフォルト
    if args.output:
        out_path = args.output
    else:
        base = os.path.splitext(args.input)[0]
        out_path = base + ".os32res"

    # .pyxres 読み込み
    if args.verbose:
        print("読み込み中: %s" % args.input)
    toml_data = load_pyxres(args.input)

    # format_version チェック
    fmt_ver = toml_data.get("format_version", 0)
    if fmt_ver > 4:
        print("警告: 未対応の format_version=%d (最大4)" % fmt_ver,
              file=sys.stderr)

    # 変換
    result = build_os32res(toml_data, verbose=args.verbose)

    # 書き出し
    with open(out_path, "wb") as f:
        f.write(result)

    print("%s → %s (%d bytes)" % (args.input, out_path, len(result)))


if __name__ == "__main__":
    main()
