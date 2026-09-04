#!/usr/bin/env python3
"""配備マニフェスト (層別 deploy.yaml) の一元定義とマージ

配備定義は所有する層ごとに分かれている。カーネル層が boot: と
ディレクトリ構造を持ち、ユーザーランド・標準アプリ・ゲームは自層の
files: だけを持つ。

このリストを二重に持つと必ず食い違う (実際 tools/deploy.yaml が層分割で
削除されたあと hostdrv_deploy.py だけが古い単一ファイルを見続け、
`make deploy` が無言の空振りになっていた)。参照側は必ずここを使うこと。
"""

import os
import sys

import yaml

PROJ_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 読み込み順 = マージ順。あとの層が boot: を持てば上書きする。
MANIFEST_RELPATHS = [
    os.path.join('build', 'core.yaml'),
    os.path.join('userland', 'deploy.yaml'),
    # 外部リポジトリ (git submodule)。未取得なら無いので load_merged が読み飛ばす。
    os.path.join('apps', 'deploy.yaml'),
    os.path.join('game', 'deploy.yaml'),
]

DEPLOY_MANIFESTS = [os.path.join(PROJ_DIR, p) for p in MANIFEST_RELPATHS]


def load_merged():
    """層ごとの配備定義をマージして返す。1 つも無ければ None。

    boot: と filesystem.directories: はカーネル層 (build/core.yaml) が持つ。
    filesystem.files: は全マニフェストを読み込み順に連結する。
    """
    merged = {'filesystem': {'directories': [], 'files': []}}
    found = False
    for path in DEPLOY_MANIFESTS:
        if not os.path.isfile(path):
            continue
        found = True
        with open(path, 'r', encoding='utf-8') as f:
            d = yaml.safe_load(f) or {}
        if 'boot' in d:
            merged['boot'] = d['boot']
        fs = d.get('filesystem') or {}
        merged['filesystem']['directories'].extend(fs.get('directories') or [])
        merged['filesystem']['files'].extend(fs.get('files') or [])
    if not found:
        print("Error: 配備定義が 1 つも見つかりません:\n  {}".format(
            "\n  ".join(DEPLOY_MANIFESTS)), file=sys.stderr)
        return None
    return merged
