#!/usr/bin/env python3
import sqlite3, sys

DB = "assets/fep.db"
TESTS = ['ひがし', 'にし', 'かん', 'かんじ', 'し', 'たいよう', 'にほん', 'きた']

conn = sqlite3.connect(DB)
for t in TESTS:
    print(f'=== {t} ===')
    for r in conn.execute('SELECT kanji, cost FROM dict WHERE yomi=? ORDER BY cost LIMIT 10', (t,)):
        print(f'  {r[0]:8s} cost={r[1]}')
    print()
conn.close()
