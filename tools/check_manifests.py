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
    "apps/deploy.yaml",
    "game/deploy.yaml",
]
PACKAGE_MANIFESTS = [
    "build/core_packages.yaml",
    "userland/package_defs.yaml",
    "apps/package_defs.yaml",
]
APP_CONF = "build/app.conf"


def built_binaries():
    out = set()
    for root in ("userland", "apps", "game"):
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


def check_doc_counts():
    """ドキュメントが書いている件数が実態と合っているか

    「17 commands」「全 11 本」の類。増減しても誰も気づかず、気づいたときには
    どれが正しいのか分からなくなる。数えられるものは数えて突き合わせる。
    """
    import re

    def count(pattern):
        return len(glob.glob(pattern))

    checks = [
        ("CLAUDE.md", r"\((\d+) commands\)",
         count("userland/cmds/*.c"), "userland/cmds/*.c"),
        ("docs/INDEX.md", r"ime等 (\d+)種",
         count("userland/cmds/*.c"), "userland/cmds/*.c"),
        ("apps/README.md", r"全 (\d+) 本",
         len([d for d in glob.glob("apps/*/") if os.path.isdir(d)]), "apps/*/"),
    ]

    bad = []
    for path, pat, actual, what in checks:
        if not os.path.isfile(path):
            continue
        m = re.search(pat, open(path, encoding="utf-8").read())
        if not m:
            bad.append((path, "-", "件数の記述が見つからない (%s)" % pat))
            continue
        claimed = int(m.group(1))
        if claimed != actual:
            bad.append((path, str(claimed),
                        "実際は %d (%s)" % (actual, what)))
    return bad


def check_shell_commands():
    """docs/07_shell.md のコマンド一覧が実装と合っているか

    組み込みは userland/shell/*.c の ShellCmd テーブル、外部コマンドは
    userland/cmds/*.c が実体。コマンドを足しても一覧に載せ忘れる。
    実際 ime と v86 が漏れ、組み込みも 9 件抜けていた。

    文書にしか無いものは「消したのに残っている」、実装にしか無いものは
    「足したのに書いていない」。どちらも検出する。
    """
    import re

    doc_path = "docs/07_shell.md"
    if not os.path.isfile(doc_path):
        return []

    doc = set()
    with open(doc_path, encoding="utf-8") as f:
        text = f.read()
    for line in text.splitlines():
        m = re.match(r"^\|\s*`([a-z0-9_.]+)`", line)
        if m:
            doc.add(m.group(1))
    # 「エイリアス: `cls`→`clear`」の形で挙げたものも記載済みとみなす
    for m in re.finditer(r"`([a-z0-9_.]+)`\s*→", text):
        doc.add(m.group(1))
    # 外部コマンドは列挙行に並ぶ
    for m in re.finditer(r"`([a-z0-9_.]+)`", text):
        doc.add(m.group(1))

    builtin = set()
    for path in glob.glob("userland/shell/*.c"):
        with open(path, encoding="utf-8", errors="replace") as f:
            for m in re.finditer(r'\{\s*"([a-z0-9_.]+)"\s*,\s*cmd_', f.read()):
                builtin.add(m.group(1))
    external = set(os.path.basename(p)[:-2] for p in glob.glob("userland/cmds/*.c"))

    missing = sorted((builtin | external) - doc)
    bad = []
    if missing:
        bad.append(("docs/07_shell.md", ", ".join(missing),
                    "実装にあるが一覧に無い"))
    return bad


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

    doc_counts = check_doc_counts()
    print("== 1b. ドキュメントの件数が実態と合っているか ==")
    if doc_counts:
        rc = 1
        for src, claimed, why in doc_counts:
            print("  [NG] {:34s} {}  ({})".format(src, claimed, why))
    else:
        print("  なし")

    shell_cmds = check_shell_commands()
    print("== 1c. シェルコマンド一覧が実装と合っているか ==")
    if shell_cmds:
        rc = 1
        for src, what, why in shell_cmds:
            print("  [NG] {:34s} {}  ({})".format(src, what, why))
    else:
        print("  なし")

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
