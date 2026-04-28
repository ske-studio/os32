#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""辞書データの品質分析スクリプト"""

import sqlite3
import os

DB_PATH = "assets/fep.db"

def main():
    conn = sqlite3.connect(DB_PATH)
    cur = conn.cursor()

    # 1. 基本統計
    total = cur.execute("SELECT count(*) FROM dict").fetchone()[0]
    unique = cur.execute(
        "SELECT count(*) FROM (SELECT DISTINCT yomi, kanji FROM dict)"
    ).fetchone()[0]
    print("=== 基本統計 ===")
    print(f"  全エントリ: {total:,}")
    print(f"  ユニーク(yomi+kanji): {unique:,}")
    print(f"  重複: {total - unique:,}")

    # 2. コスト分布
    print("\n=== コスト分布 (上位10) ===")
    for row in cur.execute(
        "SELECT cost, count(*) as c FROM dict GROUP BY cost ORDER BY c DESC LIMIT 10"
    ):
        print(f"  cost={row[0]:4d}: {row[1]:6d} entries")

    # 3. 問題例: 「かん」
    print('\n=== 「かん」候補 (ORDER BY cost) ===')
    for row in cur.execute(
        "SELECT kanji, cost FROM dict WHERE yomi='かん' ORDER BY cost LIMIT 15"
    ):
        print(f"  {row[0]:6s}  cost={row[1]}")

    # 4. 問題例: 「し」
    print('\n=== 「し」候補 (ORDER BY cost) ===')
    for row in cur.execute(
        "SELECT kanji, cost FROM dict WHERE yomi='し' ORDER BY cost LIMIT 15"
    ):
        print(f"  {row[0]:6s}  cost={row[1]}")

    # 5. 問題例: 「かんじ」
    print('\n=== 「かんじ」候補 (ORDER BY cost) ===')
    for row in cur.execute(
        "SELECT kanji, cost FROM dict WHERE yomi='かんじ' ORDER BY cost LIMIT 15"
    ):
        print(f"  {row[0]:6s}  cost={row[1]}")

    # 6. カタカナ候補の割合
    kata_count = cur.execute("""
        SELECT count(*) FROM dict
        WHERE unicode(kanji) BETWEEN 12449 AND 12534
    """).fetchone()[0]
    print(f"\n=== カタカナ表記のみの候補: {kata_count:,} / {total:,} ===")

    # 7. 同一読み・同一漢字の重複例
    print("\n=== 重複例 (yomi+kanji が同一で複数エントリ) ===")
    for row in cur.execute("""
        SELECT yomi, kanji, count(*), min(cost), max(cost)
        FROM dict GROUP BY yomi, kanji HAVING count(*) > 1
        ORDER BY count(*) DESC LIMIT 10
    """):
        print(f"  {row[0]} -> {row[1]}  x{row[2]}  cost={row[3]}-{row[4]}")

    conn.close()

if __name__ == "__main__":
    main()
