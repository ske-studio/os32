#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_kapi_version.py — KAPI バージョン表記の一致検査

KAPI のバージョンは sdk/kapi.json の "version" が唯一の情報源。
そこから生成されるヘッダと、人が手で書いたドキュメントの版数が
ずれていないかを検査する。

ずれていると「どれが本当の版か」が分からなくなり、実機で動かない
バイナリの原因究明が遠回りになる。2026-08 時点では実ヘッダ v39 に対して
README が v31、docs が v35 とまちまちだった。

使い方: python3 tools/check_kapi_version.py    (make check-kapi-version)
終了コード 0 = 一致、1 = 不一致
"""

import json
import re
import sys

SSOT = "sdk/kapi.json"

# (パス, 版数を取り出す正規表現, 説明)
TARGETS = [
    ("sdk/include/os32/os32_kapi_shared.h",
     r"#define\s+KAPI_VERSION\s+(\d+)",
     "SDK 契約ヘッダ"),
    ("README.md",
     r"KernelAPI v(\d+)",
     "README"),
    ("docs/INDEX.md",
     r"KernelAPI v(\d+) 仕様書",
     "ドキュメント索引"),
    ("docs/KAPI_SPEC.md",
     r"^# KernelAPI v(\d+) 仕様書",
     "KAPI 仕様書"),
]


def main():
    with open(SSOT, encoding="utf-8") as f:
        expected = int(json.load(f)["version"])

    bad = []
    for path, pattern, label in TARGETS:
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
        except IOError:
            bad.append((path, label, "ファイルが読めない"))
            continue
        m = re.search(pattern, text, re.M)
        if not m:
            bad.append((path, label, "版数の記述が見つからない"))
        elif int(m.group(1)) != expected:
            bad.append((path, label, "v{} (期待 v{})".format(m.group(1), expected)))

    if bad:
        print("KAPI バージョン不一致 ({} は v{})".format(SSOT, expected))
        for path, label, why in bad:
            print("  {:44s} {:16s} {}".format(path, label, why))
        print()
        print("sdk/kapi.json の version に合わせて上記を更新すること。")
        return 1

    print("KAPI バージョン一致: v{} ({} 箇所)".format(expected, len(TARGETS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
