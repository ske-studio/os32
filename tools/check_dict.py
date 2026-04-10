#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""辞書内の活用形分布と特定語の検索"""
import sys, glob, collections

input_dir = "/mnt/c/WATCOM/src/os32/assets/ipadic"
csv_files = glob.glob(f"{input_dir}/*.csv")

inflection_counts = collections.Counter()
ikite_entries = []

for f in csv_files:
    with open(f, 'r', encoding='euc-jp', errors='ignore') as fh:
        for line in fh:
            cols = line.strip().split(',')
            if len(cols) < 12:
                continue
            kanji = cols[0]
            inflection = cols[9] if len(cols) > 9 else ''
            yomi = cols[11] if len(cols) > 11 else ''
            inflection_counts[inflection] += 1
            # 「生き」を含むエントリを検索
            if '生き' in kanji or 'イキ' in yomi:
                ikite_entries.append((kanji, inflection, yomi))

print("=== 活用形の分布 (上位20) ===")
for k, v in inflection_counts.most_common(20):
    print(f"  {k:20s}: {v:6d}")

print(f"\n=== 「生き」関連エントリ ({len(ikite_entries)}件) ===")
for k, inf, y in ikite_entries[:20]:
    print(f"  {k:12s} 活用形={inf:12s} 読み={y}")
