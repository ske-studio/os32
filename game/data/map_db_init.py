#!/usr/bin/env python3
"""
map_db_init.py — テスト用マップデータ生成

/db/map.db にマスターデータを投入する。
既存DBがある場合は上書き。

使い方:
    python3 game/data/map_db_init.py            # デフォルト: game/build/db/map.db
    python3 game/data/map_db_init.py -o /path/to/map.db
"""

import sqlite3
import struct
import sys
import os


def create_schema(conn):
    """テーブルスキーマを作成"""
    cur = conn.cursor()

    cur.executescript("""
        -- マップ定義
        CREATE TABLE IF NOT EXISTS maps (
            id          INTEGER PRIMARY KEY,
            name        TEXT NOT NULL,
            width       INTEGER NOT NULL,
            height      INTEGER NOT NULL,
            layer_count INTEGER NOT NULL DEFAULT 1,
            tileset_id  INTEGER NOT NULL DEFAULT 0,
            bgm_id      INTEGER DEFAULT 0
        );

        -- タイルデータ
        CREATE TABLE IF NOT EXISTS map_tiles (
            map_id      INTEGER NOT NULL,
            layer       INTEGER NOT NULL DEFAULT 0,
            tile_data   BLOB NOT NULL,
            PRIMARY KEY (map_id, layer)
        );

        -- タイルセット定義
        CREATE TABLE IF NOT EXISTS tilesets (
            id          INTEGER PRIMARY KEY,
            name        TEXT NOT NULL,
            tile_file   TEXT NOT NULL,
            tile_count  INTEGER NOT NULL
        );

        -- タイルプロパティ
        CREATE TABLE IF NOT EXISTS tile_props (
            tileset_id  INTEGER NOT NULL,
            tile_id     INTEGER NOT NULL,
            passable    INTEGER NOT NULL DEFAULT 1,
            flags       INTEGER DEFAULT 0,
            chem_elem   INTEGER DEFAULT 0,
            chem_temp   INTEGER DEFAULT 20,
            damage      INTEGER DEFAULT 0,
            PRIMARY KEY (tileset_id, tile_id)
        );

        -- マップイベント
        CREATE TABLE IF NOT EXISTS map_events (
            id          INTEGER PRIMARY KEY,
            map_id      INTEGER NOT NULL,
            x           INTEGER NOT NULL,
            y           INTEGER NOT NULL,
            trigger     INTEGER NOT NULL DEFAULT 0,
            type        INTEGER NOT NULL DEFAULT 0,
            param       INTEGER DEFAULT 0,
            script      TEXT DEFAULT ''
        );

        -- ワープ定義
        CREATE TABLE IF NOT EXISTS warps (
            id          INTEGER PRIMARY KEY,
            src_map     INTEGER NOT NULL,
            src_x       INTEGER NOT NULL,
            src_y       INTEGER NOT NULL,
            dst_map     INTEGER NOT NULL,
            dst_x       INTEGER NOT NULL,
            dst_y       INTEGER NOT NULL,
            direction   INTEGER DEFAULT 0
        );

        -- エンカウント設定
        CREATE TABLE IF NOT EXISTS encounters (
            map_id      INTEGER NOT NULL,
            enemy_id    INTEGER NOT NULL,
            rate        INTEGER NOT NULL DEFAULT 10,
            min_steps   INTEGER DEFAULT 5
        );

        -- マップ間接続グラフ
        CREATE TABLE IF NOT EXISTS map_connections (
            from_map    INTEGER NOT NULL,
            to_map      INTEGER NOT NULL,
            direction   INTEGER NOT NULL,
            PRIMARY KEY (from_map, direction)
        );
    """)
    conn.commit()


def make_tile_blob(width, height, fill_fn):
    """
    タイルデータBLOBを生成
    fill_fn(col, row) -> tile_id (u16)
    """
    data = bytearray()
    for row in range(height):
        for col in range(width):
            tile_id = fill_fn(col, row) & 0xFFFF
            data += struct.pack('<H', tile_id)
    return bytes(data)


def populate_test_data(conn):
    """テスト用マスターデータを投入"""
    cur = conn.cursor()

    # タイルセット定義
    cur.execute("""
        INSERT INTO tilesets VALUES (0, 'field', '/gfx/field.4bpp', 64)
    """)
    cur.execute("""
        INSERT INTO tilesets VALUES (1, 'dungeon', '/gfx/dungeon.4bpp', 64)
    """)

    # タイルプロパティ (tileset_id=0: フィールド)
    # tile 0: 草 (通行可, 化学GRASS)
    cur.execute("""
        INSERT INTO tile_props VALUES (0, 0, 1, 0, 64, 20, 0)
    """)
    # tile 1: 壁 (通行不可)
    cur.execute("""
        INSERT INTO tile_props VALUES (0, 1, 0, 0, 256, 20, 0)
    """)
    # tile 2: 水 (通行不可=水上のみ, 化学WATER)
    cur.execute("""
        INSERT INTO tile_props VALUES (0, 2, 2, 0, 2, 15, 0)
    """)
    # tile 3: 木 (通行不可, 化学WOOD)
    cur.execute("""
        INSERT INTO tile_props VALUES (0, 3, 0, 0, 4, 20, 0)
    """)
    # tile 4: 氷 (通行可, 滑り, 化学ICE)
    cur.execute("""
        INSERT INTO tile_props VALUES (0, 4, 1, 2, 8, -5, 0)
    """)
    # tile 5: 沼 (通行可, 遅い, ダメージ)
    cur.execute("""
        INSERT INTO tile_props VALUES (0, 5, 1, 4, 0, 20, 5)
    """)
    # tile 6: ワープポイント (通行可, ワープフラグ)
    cur.execute("""
        INSERT INTO tile_props VALUES (0, 6, 1, 8, 0, 20, 0)
    """)
    # tile 7: セーブポイント (通行可, セーブフラグ)
    cur.execute("""
        INSERT INTO tile_props VALUES (0, 7, 1, 32, 0, 20, 0)
    """)

    # --- マップ1: テストフィールド (40x40) ---
    W, H = 40, 40

    def field_fill(col, row):
        # 外周は壁(1)、それ以外は草(0)
        if col == 0 or col == W-1 or row == 0 or row == H-1:
            return 1
        # 池 (中央付近に水タイル)
        if 10 <= col <= 13 and 10 <= row <= 13:
            return 2
        # 木 (左側に数本)
        if col == 3 and 3 <= row <= 6:
            return 3
        # 氷 (右側)
        if 18 <= col <= 20 and 5 <= row <= 8:
            return 4
        # ワープ (右下角付近)
        if col == 22 and row == 22:
            return 6
        # セーブポイント
        if col == 12 and row == 3:
            return 7
        return 0

    cur.execute("""
        INSERT INTO maps VALUES (1, 'TestField', 40, 40, 1, 0, 0)
    """)
    blob1 = make_tile_blob(W, H, field_fill)
    cur.execute("""
        INSERT INTO map_tiles VALUES (1, 0, ?)
    """, (blob1,))

    # --- マップ2: テストダンジョン (16x16) ---
    W2, H2 = 16, 16

    def dungeon_fill(col, row):
        # 外周は壁
        if col == 0 or col == W2-1 or row == 0 or row == H2-1:
            return 1
        # 通路
        if col == 1 or col == W2-2 or row == 1 or row == H2-2:
            return 0
        # 中央は壁で仕切り
        if col == 8 and row != 8:
            return 1
        return 0

    cur.execute("""
        INSERT INTO maps VALUES (2, 'TestDungeon', 16, 16, 1, 0, 0)
    """)
    blob2 = make_tile_blob(W2, H2, dungeon_fill)
    cur.execute("""
        INSERT INTO map_tiles VALUES (2, 0, ?)
    """, (blob2,))

    # --- イベント ---
    # マップ1: 宝箱 (5,5)
    cur.execute("""
        INSERT INTO map_events VALUES
            (1, 1, 5, 5, 1, 3, 100, 'treasure_01')
    """)
    # マップ1: NPC (8,4)
    cur.execute("""
        INSERT INTO map_events VALUES
            (2, 1, 8, 4, 1, 4, 0, 'npc_guide')
    """)
    # マップ1: ワープイベント (22,22) → マップ2へ
    cur.execute("""
        INSERT INTO map_events VALUES
            (3, 1, 22, 22, 0, 1, 2, 'warp_dungeon')
    """)
    # マップ2: ワープイベント (1,1) → マップ1へ
    cur.execute("""
        INSERT INTO map_events VALUES
            (4, 2, 1, 1, 0, 1, 1, 'warp_field')
    """)

    # --- ワープ ---
    # マップ1 (22,22) → マップ2 (1,1)
    cur.execute("""
        INSERT INTO warps VALUES (1, 1, 22, 22, 2, 1, 1, 2)
    """)
    # マップ2 (1,1) → マップ1 (2,2)
    cur.execute("""
        INSERT INTO warps VALUES (2, 2, 1, 1, 1, 2, 2, 0)
    """)

    # --- エンカウント ---
    # マップ1: スライム (rate=15%, min_steps=10)
    cur.execute("""
        INSERT INTO encounters VALUES (1, 1, 15, 10)
    """)
    # マップ1: ゴブリン (rate=5%, min_steps=20)
    cur.execute("""
        INSERT INTO encounters VALUES (1, 2, 5, 20)
    """)
    # マップ2: バット (rate=20%, min_steps=5)
    cur.execute("""
        INSERT INTO encounters VALUES (2, 3, 20, 5)
    """)

    # --- マップ接続 ---
    # マップ1 → マップ2 (東方向)
    cur.execute("""
        INSERT INTO map_connections VALUES (1, 2, 1)
    """)
    # マップ2 → マップ1 (西方向)
    cur.execute("""
        INSERT INTO map_connections VALUES (2, 1, 3)
    """)

    conn.commit()


def main():
    """エントリポイント"""
    out_path = 'game/build/db/map.db'
    if len(sys.argv) > 2 and sys.argv[1] == '-o':
        out_path = sys.argv[2]

    # 出力ディレクトリ作成
    out_dir = os.path.dirname(out_path)
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir)

    # 既存ファイル削除
    if os.path.exists(out_path):
        os.remove(out_path)

    print(f"Creating map database: {out_path}")

    conn = sqlite3.connect(out_path)
    create_schema(conn)
    populate_test_data(conn)

    # 統計表示
    cur = conn.cursor()
    cur.execute("SELECT COUNT(*) FROM maps")
    n_maps = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM tile_props")
    n_props = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM map_events")
    n_events = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM warps")
    n_warps = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM encounters")
    n_enc = cur.fetchone()[0]

    print(f"  Maps:       {n_maps}")
    print(f"  TileProps:  {n_props}")
    print(f"  Events:     {n_events}")
    print(f"  Warps:      {n_warps}")
    print(f"  Encounters: {n_enc}")

    # BLOBサイズ確認
    cur.execute("SELECT map_id, layer, LENGTH(tile_data) FROM map_tiles")
    for row in cur.fetchall():
        print(f"  map_tiles: map_id={row[0]} layer={row[1]} size={row[2]}B")

    conn.close()
    print("Done.")


if __name__ == '__main__':
    main()
