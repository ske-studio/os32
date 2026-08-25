#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_manifests.py — 配備マニフェストと app.conf の参照先を検査する

3 種類の食い違いを検出する。いずれも「静かに壊れる」たちの悪い部類:

1. 配備定義が挙げているのにビルドされないファイル
   → NHD 上に古いバイナリが残り続ける。KAPI レイアウトが変わると
     旧バイナリの KAPI 呼び出しが別関数へ飛び、exit 後の jmp $ で
     永久スピンして rshell ごと沈黙する (deploy.yaml 冒頭の警告)。

2. app.conf のキーが実在のターゲットに一致していない
   → 既定値 (api 7 / heap 64KB) で出荷される。設定したつもりの
     ヒープサイズが効かない。

3. ビルドされるのに配備定義に載っていないバイナリ
   → 実機に届かない。

先に make all を通してから実行すること。
"""

import glob
import os
import sys

try:
    import yaml
except ImportError:
    print("PyYAML が必要: pip install pyyaml", file=sys.stderr)
    sys.exit(2)

# 配備定義は所有する層ごとに分かれている。
DEPLOY_MANIFESTS = [
    "build/core.yaml",
    "userland/deploy.yaml",
    "game/deploy.yaml",
]
PACKAGE_MANIFESTS = [
    "build/core_packages.yaml",
    "userland/package_defs.yaml",
    "game/package_defs.yaml",
]
APP_CONF = "build/app.conf"


def built_binaries():
    out = set()
    for root in ("userland", "game"):
        for p in glob.glob(root + "/**/*.bin", recursive=True):
            out.add(p)
    return out


def check_missing_hosts():
    """戻り値: (エラー, 警告)

    glob は「あれば配る」という書き方なので一致ゼロでもエラーにしない。
    実際 manga は *.mgx と *.MGX を両方書いて大小文字を吸収しており、
    どちらかは必ず一致しない。実ファイル指定の欠落だけがエラー。
    """
    bad = []
    warn = []
    for path in DEPLOY_MANIFESTS:
        if not os.path.isfile(path):
            bad.append((path, "-", "マニフェストがない"))
            continue
        with open(path, encoding="utf-8") as f:
            d = yaml.safe_load(f) or {}
        for e in (d.get("filesystem") or {}).get("files") or []:
            h = e["host"]
            if e.get("type") == "glob" or "*" in h:
                if not glob.glob(h):
                    warn.append((path, h, "glob 一致なし"))
            elif not os.path.isfile(h):
                bad.append((path, h, "ファイルなし"))

    for path in PACKAGE_MANIFESTS:
        if not os.path.isfile(path):
            bad.append((path, "-", "マニフェストがない"))
            continue
        with open(path, encoding="utf-8") as f:
            p = yaml.safe_load(f) or {}
        for pkg, body in p.items():
            for e in (body or {}).get("files", []):
                h = e["host"]
                if "*" in h:
                    if not glob.glob(h):
                        warn.append((path + " [" + pkg + "]", h, "glob 一致なし"))
                elif not os.path.isfile(h):
                    bad.append((path + " [" + pkg + "]", h, "ファイルなし"))
    return bad, warn


def check_app_conf(bins):
    """app.conf のキーが実在の .bin に対応しているか"""
    bad = []
    with open(APP_CONF, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            key = s.split()[0]
            if key + ".bin" not in bins:
                bad.append((lineno, key))
    return bad


def check_undeployed(bins):
    deployed = set()
    for path in DEPLOY_MANIFESTS:
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            d = yaml.safe_load(f) or {}
        for e in (d.get("filesystem") or {}).get("files") or []:
            h = e["host"]
            if "*" in h:
                deployed.update(glob.glob(h))
            else:
                deployed.add(h)
    return sorted(bins - deployed)


def main():
    bins = built_binaries()
    if not bins:
        print("ビルド成果物が見つからない。先に make all を実行すること。",
              file=sys.stderr)
        return 2

    rc = 0

    missing, warn = check_missing_hosts()
    print("== 1. マニフェストが挙げているのに存在しないファイル ==")
    if missing:
        rc = 1
        for src, h, why in missing:
            print("  [NG] {:34s} {}  ({})".format(src, h, why))
    else:
        print("  なし")
    if warn:
        for src, h, why in warn:
            print("  [--] {:34s} {}  ({})".format(src, h, why))

    bad_keys = check_app_conf(bins)
    print("== 2. 実在ターゲットに一致しない app.conf のキー ==")
    if bad_keys:
        rc = 1
        for lineno, key in bad_keys:
            print("  [NG] {}:{}  '{}' に対応する .bin がない".format(
                APP_CONF, lineno, key))
    else:
        print("  なし")

    undeployed = check_undeployed(bins)
    print("== 3. ビルドされるが配備定義に載っていないバイナリ ==")
    if undeployed:
        for h in undeployed:
            print("  [--] {}".format(h))
        print("  ({} 件。意図的に配備しないものはここに出てよい)".format(
            len(undeployed)))
    else:
        print("  なし")

    return rc


if __name__ == "__main__":
    sys.exit(main())
