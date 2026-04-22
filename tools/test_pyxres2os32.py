#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_pyxres2os32.py — pyxres2os32.py の検証テスト

ミニマルな .pyxres を生成し、変換後のバイナリを検証する。
"""

import sys
import os
import struct
import zipfile
import tempfile

# テスト用に変換スクリプトをインポート
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pyxres2os32

# ========================================================================
#  テスト用 .pyxres 生成
# ========================================================================

def create_test_pyxres(path):
    """ミニマルな .pyxres ファイルを生成する。

    内容:
    - イメージバンク0: 8x8 領域に既知のパターン (残りは0)
      左上 4x4 を色1, 右上 4x4 を色2, 左下 4x4 を色4, 右下 4x4 を色8
    - イメージバンク1: 全て0 (空、スキップされるべき)
    - タイルマップ0: 2x2, バンク0参照, タイル (0,0),(1,0),(0,1),(1,1)
    - タイルマップ1: 全て0 (空、スキップされるべき)
    - サウンド0: C4 を4ノート繰り返し, speed=30
    - サウンド1: 空 (スキップされるべき)
    - ミュージック0: ch0=[0], ch1=[]
    - ミュージック1: 空 (スキップされるべき)
    """

    # イメージバンク0: 8x8 パターン
    # 行0-3: [1,1,1,1, 2,2,2,2, 0] (末尾0で残りを充填)
    # 行4-7: [4,4,4,4, 8,8,8,8, 0]
    # 最終行(行8): [0] → 末尾行圧縮で行8-255 が全0
    img0_data = []
    for y in range(4):
        img0_data.append([1, 1, 1, 1, 2, 2, 2, 2, 0])
    for y in range(4):
        img0_data.append([4, 4, 4, 4, 8, 8, 8, 8, 0])
    img0_data.append([0])  # 行8以降は全0 (末尾行圧縮)

    # イメージバンク1: 空 (全0)
    img1_data = [[0]]

    # タイルマップ0: 2x2, imgsrc=0
    # インターリーブ形式: [tx0, ty0, tx1, ty1]
    tm0_data = [
        [0, 0, 1, 0],  # 行0: タイル(0,0) と タイル(1,0)
        [0, 1, 1, 1],  # 行1: タイル(0,1) と タイル(1,1)
    ]

    # タイルマップ1: 空
    tm1_data = [[0]]

    # サウンド0: C4(=48) を4ノート, speed=30
    snd0 = {
        "notes": [48, 48, 48, 48],
        "tones": [1],        # Square (末尾圧縮: 全てSquare)
        "volumes": [6],      # 末尾圧縮
        "effects": [0],      # 末尾圧縮: None
        "speed": 30,
    }

    # サウンド1: 空
    snd1 = {
        "notes": [],
        "tones": [],
        "volumes": [],
        "effects": [],
        "speed": 30,
    }

    # ミュージック0: ch0 でサウンド0を再生
    mus0 = {"seqs": [[0], []]}

    # ミュージック1: 空
    mus1 = {"seqs": []}

    # TOML テキスト構築 (手書き)
    toml_lines = []
    toml_lines.append("format_version = 1")
    toml_lines.append("")

    # イメージバンク0
    toml_lines.append("[[images]]")
    toml_lines.append("width = 256")
    toml_lines.append("height = 256")
    toml_lines.append("data = [")
    for row in img0_data:
        toml_lines.append("  %s," % repr(row))
    toml_lines.append("]")
    toml_lines.append("")

    # イメージバンク1 (空)
    toml_lines.append("[[images]]")
    toml_lines.append("width = 256")
    toml_lines.append("height = 256")
    toml_lines.append("data = [")
    for row in img1_data:
        toml_lines.append("  %s," % repr(row))
    toml_lines.append("]")
    toml_lines.append("")

    # タイルマップ0
    toml_lines.append("[[tilemaps]]")
    toml_lines.append("width = 2")
    toml_lines.append("height = 2")
    toml_lines.append("imgsrc = 0")
    toml_lines.append("data = [")
    for row in tm0_data:
        toml_lines.append("  %s," % repr(row))
    toml_lines.append("]")
    toml_lines.append("")

    # タイルマップ1 (空)
    toml_lines.append("[[tilemaps]]")
    toml_lines.append("width = 256")
    toml_lines.append("height = 256")
    toml_lines.append("imgsrc = 0")
    toml_lines.append("data = [")
    for row in tm1_data:
        toml_lines.append("  %s," % repr(row))
    toml_lines.append("]")
    toml_lines.append("")

    # サウンド0
    toml_lines.append("[[sounds]]")
    toml_lines.append("notes = %s" % repr(snd0["notes"]))
    toml_lines.append("tones = %s" % repr(snd0["tones"]))
    toml_lines.append("volumes = %s" % repr(snd0["volumes"]))
    toml_lines.append("effects = %s" % repr(snd0["effects"]))
    toml_lines.append("speed = %d" % snd0["speed"])
    toml_lines.append("")

    # サウンド1 (空)
    toml_lines.append("[[sounds]]")
    toml_lines.append("notes = %s" % repr(snd1["notes"]))
    toml_lines.append("tones = %s" % repr(snd1["tones"]))
    toml_lines.append("volumes = %s" % repr(snd1["volumes"]))
    toml_lines.append("effects = %s" % repr(snd1["effects"]))
    toml_lines.append("speed = %d" % snd1["speed"])
    toml_lines.append("")

    # ミュージック0
    toml_lines.append("[[musics]]")
    toml_lines.append("seqs = %s" % repr(mus0["seqs"]))
    toml_lines.append("")

    # ミュージック1 (空)
    toml_lines.append("[[musics]]")
    toml_lines.append("seqs = %s" % repr(mus1["seqs"]))
    toml_lines.append("")

    toml_text = "\n".join(toml_lines)

    # ZIP 化
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("pyxel_resource.toml", toml_text)

    return toml_text


# ========================================================================
#  検証関数
# ========================================================================

def assert_eq(name, actual, expected):
    """値の一致を検証する。"""
    if actual != expected:
        print("  FAIL: %s: expected=%r, actual=%r" % (name, expected, actual))
        return False
    return True


def verify_header(data):
    """ヘッダのバイナリ検証。"""
    print("ヘッダ検証:")
    ok = True

    magic = data[0:4]
    ok &= assert_eq("magic", magic, b"PX32")

    version = struct.unpack_from("<I", data, 4)[0]
    ok &= assert_eq("version", version, 1)

    img_count = struct.unpack_from("<H", data, 8)[0]
    ok &= assert_eq("img_count", img_count, 1)  # バンク0のみ (バンク1は空)

    tm_count = struct.unpack_from("<H", data, 10)[0]
    ok &= assert_eq("tm_count", tm_count, 1)  # マップ0のみ

    snd_count = struct.unpack_from("<H", data, 12)[0]
    ok &= assert_eq("snd_count", snd_count, 1)  # サウンド0のみ

    mus_count = struct.unpack_from("<H", data, 14)[0]
    ok &= assert_eq("mus_count", mus_count, 1)  # ミュージック0のみ

    if ok:
        print("  OK")
    return ok


def verify_image_section(data):
    """イメージバンク0 のプレーナー変換検証。

    入力パターン (8x8 領域):
      行0-3: 色1 (0001b) ×4, 色2 (0010b) ×4
      行4-7: 色4 (0100b) ×4, 色8 (1000b) ×4

    プレーナー変換後 (バイト0のみ、x=0..7):
      Plane B (bit0): 行0-3=[1111_0000]=0xF0, 行4-7=[0000_0000]=0x00
      Plane R (bit1): 行0-3=[0000_1111]=0x0F, 行4-7=[0000_0000]=0x00
      Plane G (bit2): 行0-3=[0000_0000]=0x00, 行4-7=[1111_0000]=0xF0
      Plane I (bit3): 行0-3=[0000_0000]=0x00, 行4-7=[0000_1111]=0x0F
    """
    print("イメージバンク検証:")
    ok = True

    img_offset = struct.unpack_from("<I", data, 0x10)[0]
    plane_size = pyxres2os32.IMG_PLANE_SIZE  # 8192
    bpl = pyxres2os32.IMG_BPL  # 32

    # Plane B (bit0): 行0, バイト0 = 0xF0 (色1の bit0 がセット)
    plane_b_start = img_offset
    for y in range(4):
        val = data[plane_b_start + y * bpl]
        ok &= assert_eq("PlaneB[%d][0]" % y, val, 0xF0)
    for y in range(4, 8):
        val = data[plane_b_start + y * bpl]
        ok &= assert_eq("PlaneB[%d][0]" % y, val, 0x00)

    # Plane R (bit1): 行0, バイト0 = 0x0F (色2の bit1 がセット)
    plane_r_start = img_offset + plane_size
    for y in range(4):
        val = data[plane_r_start + y * bpl]
        ok &= assert_eq("PlaneR[%d][0]" % y, val, 0x0F)
    for y in range(4, 8):
        val = data[plane_r_start + y * bpl]
        ok &= assert_eq("PlaneR[%d][0]" % y, val, 0x00)

    # Plane G (bit2): 行4, バイト0 = 0xF0 (色4の bit2 がセット)
    plane_g_start = img_offset + plane_size * 2
    for y in range(4):
        val = data[plane_g_start + y * bpl]
        ok &= assert_eq("PlaneG[%d][0]" % y, val, 0x00)
    for y in range(4, 8):
        val = data[plane_g_start + y * bpl]
        ok &= assert_eq("PlaneG[%d][0]" % y, val, 0xF0)

    # Plane I (bit3): 行4, バイト0 = 0x0F (色8の bit3 がセット)
    plane_i_start = img_offset + plane_size * 3
    for y in range(4):
        val = data[plane_i_start + y * bpl]
        ok &= assert_eq("PlaneI[%d][0]" % y, val, 0x00)
    for y in range(4, 8):
        val = data[plane_i_start + y * bpl]
        ok &= assert_eq("PlaneI[%d][0]" % y, val, 0x0F)

    # 8ピクセル目以降 (バイト1) は全て0
    for p in range(4):
        plane_start = img_offset + plane_size * p
        for y in range(8):
            val = data[plane_start + y * bpl + 1]
            ok &= assert_eq("Plane%d[%d][1]" % (p, y), val, 0x00)

    if ok:
        print("  OK")
    return ok


def verify_tilemap_section(data):
    """タイルマップ0 の検証。"""
    print("タイルマップ検証:")
    ok = True

    tm_offset = struct.unpack_from("<I", data, 0x14)[0]

    imgsrc = data[tm_offset]
    ok &= assert_eq("imgsrc", imgsrc, 0)

    padding = data[tm_offset + 1]
    ok &= assert_eq("padding", padding, 0)

    width = struct.unpack_from("<H", data, tm_offset + 2)[0]
    ok &= assert_eq("width", width, 2)

    height = struct.unpack_from("<H", data, tm_offset + 4)[0]
    ok &= assert_eq("height", height, 2)

    # タイルデータ: (tx, ty) ペア
    tile_data_off = tm_offset + 6
    # 行0: (0,0), (1,0)
    ok &= assert_eq("tile[0,0].tx", data[tile_data_off + 0], 0)
    ok &= assert_eq("tile[0,0].ty", data[tile_data_off + 1], 0)
    ok &= assert_eq("tile[0,1].tx", data[tile_data_off + 2], 1)
    ok &= assert_eq("tile[0,1].ty", data[tile_data_off + 3], 0)
    # 行1: (0,1), (1,1)
    ok &= assert_eq("tile[1,0].tx", data[tile_data_off + 4], 0)
    ok &= assert_eq("tile[1,0].ty", data[tile_data_off + 5], 1)
    ok &= assert_eq("tile[1,1].tx", data[tile_data_off + 6], 1)
    ok &= assert_eq("tile[1,1].ty", data[tile_data_off + 7], 1)

    if ok:
        print("  OK")
    return ok


def verify_sound_section(data):
    """サウンド0 の検証。"""
    print("サウンド検証:")
    ok = True

    snd_offset = struct.unpack_from("<I", data, 0x18)[0]

    speed = struct.unpack_from("<H", data, snd_offset)[0]
    ok &= assert_eq("speed", speed, 30)

    note_count = struct.unpack_from("<H", data, snd_offset + 2)[0]
    ok &= assert_eq("note_count", note_count, 4)

    # notes: C4=48 × 4
    notes_off = snd_offset + 4
    for i in range(4):
        n = struct.unpack_from("b", data, notes_off + i)[0]
        ok &= assert_eq("note[%d]" % i, n, 48)

    # tones: 1 (Square) × 4 (末尾圧縮展開)
    tones_off = notes_off + 4
    for i in range(4):
        t = data[tones_off + i]
        ok &= assert_eq("tone[%d]" % i, t, 1)

    # volumes: 6 × 4
    vol_off = tones_off + 4
    for i in range(4):
        v = data[vol_off + i]
        ok &= assert_eq("volume[%d]" % i, v, 6)

    # effects: 0 × 4
    eff_off = vol_off + 4
    for i in range(4):
        e = data[eff_off + i]
        ok &= assert_eq("effect[%d]" % i, e, 0)

    if ok:
        print("  OK")
    return ok


def verify_music_section(data):
    """ミュージック0 の検証。"""
    print("ミュージック検証:")
    ok = True

    mus_offset = struct.unpack_from("<I", data, 0x1C)[0]

    ch_count = struct.unpack_from("<H", data, mus_offset)[0]
    ok &= assert_eq("channel_count", ch_count, 1)

    # ch0: seq_count=1, seq=[0]
    seq_count = struct.unpack_from("<H", data, mus_offset + 2)[0]
    ok &= assert_eq("ch0_seq_count", seq_count, 1)

    snd_idx = struct.unpack_from("<H", data, mus_offset + 4)[0]
    ok &= assert_eq("ch0_seq[0]", snd_idx, 0)

    if ok:
        print("  OK")
    return ok


# ========================================================================
#  メイン
# ========================================================================

def main():
    print("=" * 60)
    print("pyxres2os32 テスト")
    print("=" * 60)

    # 一時ファイルで作業
    tmpdir = tempfile.mkdtemp(prefix="pyxres_test_")
    pyxres_path = os.path.join(tmpdir, "test.pyxres")
    os32res_path = os.path.join(tmpdir, "test.os32res")

    try:
        # 1. テスト用 .pyxres 生成
        print("\n1. テスト用 .pyxres 生成中...")
        toml_text = create_test_pyxres(pyxres_path)
        print("  生成: %s (%d bytes)" % (pyxres_path, os.path.getsize(pyxres_path)))

        # 2. 変換実行
        print("\n2. 変換中...")
        toml_data = pyxres2os32.load_pyxres(pyxres_path)
        result = pyxres2os32.build_os32res(toml_data, verbose=True)

        with open(os32res_path, "wb") as f:
            f.write(result)
        print("  出力: %s (%d bytes)" % (os32res_path, len(result)))

        # 3. バイナリ検証
        print("\n3. バイナリ検証中...")
        with open(os32res_path, "rb") as f:
            data = f.read()

        all_ok = True
        print("")
        all_ok &= verify_header(data)
        print("")
        all_ok &= verify_image_section(data)
        print("")
        all_ok &= verify_tilemap_section(data)
        print("")
        all_ok &= verify_sound_section(data)
        print("")
        all_ok &= verify_music_section(data)

        print("")
        print("=" * 60)
        if all_ok:
            print("結果: 全テスト PASS ✓")
        else:
            print("結果: 一部テスト FAIL ✗")
            sys.exit(1)
        print("=" * 60)

    finally:
        # 一時ファイルの後始末
        for f in [pyxres_path, os32res_path]:
            if os.path.exists(f):
                os.unlink(f)
        if os.path.exists(tmpdir):
            os.rmdir(tmpdir)


if __name__ == "__main__":
    main()
