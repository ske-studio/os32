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


def check_spec_table():
    """docs/KAPI_SPEC.md の関数表が kapi.json と一致しているか

    表は手書きなので、関数を足したときに更新を忘れる。実際 v86 系の 4 本が
    丸ごと抜け、その分データフィールドのオフセットが 16 バイト手前へ
    ずれていた。オフセットは外部プログラムが構造体を引く位置そのものなので、
    ドキュメントを信じて実装すると別の関数を呼ぶ。

    名前・並び順・オフセット・シグネチャの 4 つを突き合わせる。
    """
    import os

    proj = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    spec = os.path.join(proj, "docs", "KAPI_SPEC.md")
    kapi = os.path.join(proj, "sdk", "kapi.json")
    if not (os.path.isfile(spec) and os.path.isfile(kapi)):
        return ["KAPI_SPEC.md または kapi.json が見つからない"]

    with open(kapi, encoding="utf-8") as f:
        api = json.load(f)["api"]

    row = re.compile(r"^\|\s*(0x[0-9A-Fa-f]+)\s*\|\s*([A-Za-z_][A-Za-z0-9_]*)"
                     r"\s*\|\s*`([^`]*)`\s*\|")
    rows = []
    with open(spec, encoding="utf-8") as f:
        for line in f:
            m = row.match(line)
            if m:
                rows.append((int(m.group(1), 16), m.group(2), m.group(3)))

    end = 0x08 + 4 * len(api)          # 関数領域の終端 = データフィールドの先頭
    want = [(0x08 + 4 * i, e["name"]) for i, e in enumerate(api)]
    got = [(o, n) for o, n, _ in rows if o < end]

    problems = []
    wn = {n for _, n in want}
    gn = {n for _, n in got}
    missing = [n for _, n in want if n not in gn]
    extra = [n for _, n in got if n not in wn]
    if missing:
        problems.append("KAPI_SPEC.md に無い関数: %s" % ", ".join(missing))
    if extra:
        problems.append("KAPI_SPEC.md にのみある関数: %s" % ", ".join(extra))

    if not problems and want != got:
        for i, (a, b) in enumerate(zip(want, got)):
            if a != b:
                problems.append("並び/オフセットのずれ: idx %d 期待 0x%X %s / 文書 0x%X %s"
                                % (i, a[0], a[1], b[0], b[1]))
                break

    sig = dict((n, s) for _, n, s in rows)
    for e in api:
        w = "%s(%s)" % (e["ret"], ", ".join(e["args"]) or "void")
        g = sig.get(e["name"])
        if g is not None and g.replace(" ", "") != w.replace(" ", ""):
            problems.append("シグネチャ不一致 %s: json=%s / doc=%s"
                            % (e["name"], w, g))

    # データフィールドは関数領域の直後に並ぶ
    for i, df in enumerate(json.load(open(kapi, encoding="utf-8"))["data_fields"]):
        off = end + 4 * i
        found = [o for o, n, _ in rows if n == df["name"]]
        if not found:
            problems.append("データフィールドが KAPI_SPEC.md に無い: %s" % df["name"])
        elif found[0] != off:
            problems.append("データフィールドのオフセットずれ %s: 期待 0x%X / 文書 0x%X"
                            % (df["name"], off, found[0]))
    return problems


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

    spec = check_spec_table()
    if spec:
        print("")
        print("KAPI_SPEC.md の関数表が kapi.json と食い違っている")
        for s in spec:
            print("  - {}".format(s))
        print("")
        print("表は手書きなので、関数を足したら同じコミットで更新すること。")
        return 1

    print("KAPI_SPEC.md の関数表: kapi.json と一致")
    return 0


if __name__ == "__main__":
    sys.exit(main())
