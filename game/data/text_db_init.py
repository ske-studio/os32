#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
text_db_init.py — libos32text テスト用データベース生成

テスト・検証専用のサンプルデータを生成する。
実際のゲーム用DBは各ゲームのビルドパイプラインで生成すること。

使い方:
    python3 game/data/text_db_init.py

出力:
    game/build/db/text.db
"""

import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(__file__), '..', 'build', 'db', 'text.db')

def create_tables(conn):
    """テーブル作成"""
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS msg_groups (
            id       INTEGER PRIMARY KEY,
            name     TEXT NOT NULL,
            category TEXT
        );

        CREATE TABLE IF NOT EXISTS messages (
            id       INTEGER PRIMARY KEY,
            group_id INTEGER NOT NULL DEFAULT 0,
            seq      INTEGER NOT NULL DEFAULT 0,
            speaker  TEXT,
            text     TEXT NOT NULL,
            speed    INTEGER NOT NULL DEFAULT 2
        );

        CREATE INDEX IF NOT EXISTS idx_messages_group
            ON messages(group_id, seq);
    """)

def insert_test_data(conn):
    """テスト用サンプルデータ"""
    cur = conn.cursor()

    # === グループ定義 ===
    groups = [
        (1, 'scene_opening', 'scene'),
        (2, 'npc_villager',  'npc'),
        (3, 'system',        'system'),
    ]
    cur.executemany("INSERT INTO msg_groups (id, name, category) VALUES (?, ?, ?)", groups)

    # === メッセージデータ ===
    messages = [
        # 単体メッセージ (グループなし)
        (1, 0, 0, None,
         "Hello, World!",
         2),

        # 日本語テスト
        (2, 0, 0, None,
         "これはテストメッセージです。",
         2),

        # 変数埋め込みテスト
        (3, 0, 0, None,
         "{0}は {1} を手に入れた！",
         2),

        # ページ分割テスト (\p)
        (4, 0, 0, None,
         "最初のページです。\\p2ページ目です。\\p最後のページ。",
         2),

        # 一時停止テスト (\w)
        (5, 0, 0, None,
         "3...\\w30 2...\\w30 1...\\w30 スタート！",
         1),

        # 話者名テスト
        (6, 0, 0, "村人A",
         "ようこそ、旅の方。\\pこの村は平和ですよ。",
         2),

        # === シーン: オープニング (グループ1) ===
        (10, 1, 0, None,
         "むかしむかし、あるところに……",
         3),
        (11, 1, 1, None,
         "{0}という名の勇者がおりました。",
         2),
        (12, 1, 2, "長老",
         "さあ、冒険の旅に出るのじゃ。\\p気をつけてな。",
         2),

        # === NPC: 村人 (グループ2) ===
        (20, 2, 0, "村人B",
         "この先に洞窟があるよ。",
         2),
        (21, 2, 1, "村人B",
         "でも、モンスターが出るから\\p気をつけてね。",
         2),

        # === システムメッセージ (グループ3) ===
        (30, 3, 0, None,
         "{0} を {1}個 手に入れた！",
         1),
    ]
    cur.executemany(
        "INSERT INTO messages (id, group_id, seq, speaker, text, speed) VALUES (?, ?, ?, ?, ?, ?)",
        messages
    )

    conn.commit()

def main():
    # 既存ファイルを削除して再生成
    if os.path.exists(DB_PATH):
        os.remove(DB_PATH)

    os.makedirs(os.path.dirname(os.path.abspath(DB_PATH)), exist_ok=True)

    conn = sqlite3.connect(DB_PATH)
    create_tables(conn)
    insert_test_data(conn)

    # 統計表示
    cur = conn.cursor()
    cur.execute("SELECT COUNT(*) FROM msg_groups")
    n_groups = cur.fetchone()[0]
    cur.execute("SELECT COUNT(*) FROM messages")
    n_msgs = cur.fetchone()[0]

    print(f"=== text_db_init.py ===")
    print(f"  Output: {DB_PATH}")
    print(f"  Groups: {n_groups}")
    print(f"  Messages: {n_msgs}")
    print(f"  Size: {os.path.getsize(DB_PATH)} bytes")

    conn.close()

if __name__ == '__main__':
    main()
