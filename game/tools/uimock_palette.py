#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uimock_palette.py — 退色レトロ地図風パレットの定義

PC-98 のパレットは書き換えられる (KAPI: gfx_set_palette(idx, r, g, b)、
各成分 0-15)。カーネル既定は 0/7/15 しか使わない原色パレットなので
派手になる。ゲーム起動時にこの16色を流し込む前提で設計した。

配色の方針:
  - マップ背景は「海 + 陸3段」の4色で成立させる (退色モスグリーン系)
  - 建物・キャラは背景より一段明るい/彩度のある色を少数だけ使い、
    地図の上で確実に目立たせる
  - UI (窓・文字) は残りのスロットに寄せる

添字は既存コードの慣習 (0=黒, 15=白) をなるべく壊さないように置いた。
"""

# idx: (r, g, b) 各 0-15  /  用途
#
# 古地図 (羊皮紙に刷った日本地図) の色合い。
#   陸  = クリーム〜生成りの紙。面積が一番広いので最も明るい
#   海  = くすんだスレートブルー。彩度を落として紙に馴染ませる
#   陰影 = セピア〜オリーブ。山地の起伏はこれで描く
#   線・文字 = 濃いセピア (真っ黒にすると印刷物に見えない)
RETRO_MAP = [
    #  0 濃セピア。輪郭・文字・影 (黒より柔らかい)
    (2,  2,  1),
    #  1 海 (深)。スレートブルー
    (4,  6,  6),
    #  2 海 (浅) / 水際
    (6,  8,  8),
    #  3 陸 (陰) セピア。山地の陰・ハッチング
    (9,  8,  5),
    #  4 陸 (地) 羊皮紙 — 面積が一番広い基本色
    (13, 12, 9),
    #  5 陸 (明) 生成り。平地のハイライト・紙の明るい部分
    (14, 14, 11),
    #  6 道 (褪せた朱土)
    (11, 9,  6),
    #  7 岩・石垣 (灰茶)
    (8,  8,  7),
    #  8 森 (くすんだオリーブ)
    (7,  8,  4),
    #  9 プレイヤー青 (藍。紙の上で沈まない程度に残す)
    (4,  6, 11),
    # 10 プレイヤー赤 (朱)
    (12, 5,  3),
    # 11 プレイヤー紫 / 魔法 (退色した藤)
    (9,  6, 10),
    # 12 プレイヤー緑 (苔緑。陸の色と明度で分離)
    (5,  9,  4),
    # 13 水色 / 神社 (褪せた浅葱)
    (7, 10, 11),
    # 14 プレイヤー黄 / 強調 (山吹)
    (14, 11, 4),
    # 15 白 (紙の白。わずかに黄味)
    (15, 15, 13),
]

# 用途の別名 (実装時にそのまま #define へ移せる)
PAL_BLACK      = 0
PAL_SEA_DEEP   = 1
PAL_SEA_SHALLOW= 2
PAL_LAND_LOW   = 3   # 陸の陰 (セピア)
PAL_LAND_MID   = 4   # 陸の地 (羊皮紙)
PAL_LAND_HIGH  = 5   # 陸の明 (生成り)
PAL_ROAD       = 6
PAL_ROCK       = 7
PAL_FOREST     = 8
PAL_P_BLUE     = 9
PAL_P_RED      = 10
PAL_MAGIC      = 11
PAL_P_GREEN    = 12
PAL_SKY        = 13
PAL_P_YELLOW   = 14
PAL_WHITE      = 15

# マップ背景を構成する4色 (これだけで地図が成立するように配色してある)
# 海1 + 陸3 (陰 / 地 / 明)。陸の3段で起伏を描く
MAP_BASE_4 = [PAL_SEA_DEEP, PAL_LAND_LOW, PAL_LAND_MID, PAL_LAND_HIGH]


def to_rgb888(pal4):
    """4bit/ch を 8bit/ch へ"""
    return [(r * 17, g * 17, b * 17) for (r, g, b) in pal4]


def c_source():
    """実装用の C 配列を吐く"""
    out = ['/* 退色レトロ地図風パレット。glue_init() で流し込む */',
           'static const u8 g_palette[16][3] = {']
    names = ['BLACK', 'SEA_DEEP', 'SEA_SHALLOW', 'LAND_LOW', 'LAND_MID',
             'LAND_HIGH', 'ROAD', 'ROCK', 'FOREST', 'P_BLUE', 'P_RED',
             'MAGIC', 'P_GREEN', 'SKY', 'P_YELLOW', 'WHITE']
    for i, (r, g, b) in enumerate(RETRO_MAP):
        out.append('    {%2d, %2d, %2d },   /* %2d %s */' % (r, g, b, i, names[i]))
    out.append('};')
    return '\n'.join(out)


if __name__ == '__main__':
    print(c_source())
