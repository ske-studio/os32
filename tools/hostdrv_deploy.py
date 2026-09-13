#!/usr/bin/env python3
"""
hostdrv_deploy.py — HostDrv共有ディレクトリへのデプロイスクリプト

NP21/W の HostDrv 機能を利用し、ビルド成果物をホスト共有ディレクトリ
(C:/os32 = WSL: /mnt/c/os32) に配置する。
ゲストOS32は /host マウントポイント経由で直接アクセスできる。

sudo 不要。NHDイメージ操作不要。プログラム変更時は NP21/W 再起動不要。
カーネル変更時のみ nhd_deploy.py でブート領域書き込み + 再起動が必要。

使い方:
  python3 hostdrv_deploy.py sync [--tag TAG]   — deploy.yaml に基づくデプロイ
  python3 hostdrv_deploy.py diff               — ビルド成果物との差分表示
  python3 hostdrv_deploy.py clean              — HostDrvディレクトリをクリア
  python3 hostdrv_deploy.py ls [path]          — HostDrvディレクトリ一覧
"""

import sys
import os
import shutil
import glob as globmod
import yaml
import filecmp

# 通常配備が /etc/settings.db* を作らない・上書きしない・消さないための共通判定
# (票 S0-D / D0)。HostDrv は hsync でそのまま NHD へ流れるので、ここに古い
# settings.db が置かれるだけで本体を潰す道ができる。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deploy_protect as protect

# === パス設定 ===

# プロジェクトルート (tools/ の親ディレクトリ)
PROJ_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 配備定義は層別 (build/core.yaml, userland/, apps/, game/)。実体は
# tools/deploy_manifests.py。以前はここが削除済みの tools/deploy.yaml を
# 単独で見ており、`make deploy` が無言で何もしない状態だった。
from deploy_manifests import DEPLOY_MANIFESTS, load_merged as _load_merged


def _resolve_hostdrv_dir():
    """HOSTDRV_DIRを .env から解決する"""
    if os.environ.get('HOSTDRV_DIR'):
        return os.environ['HOSTDRV_DIR']
    for env_file in ['.env', '.env.sample']:
        env_path = os.path.join(PROJ_DIR, env_file)
        if os.path.isfile(env_path):
            with open(env_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if line.startswith('HOSTDRV_DIR='):
                        return line.split('=', 1)[1].strip()
    return "/mnt/c/os32"


HOSTDRV_DIR = _resolve_hostdrv_dir()


def guard_dest(guest_path, host_src=None):
    """最終パスを確定して保護判定する (HostDrv 側の共通入口)。

    Returns: (dest_abs, state)  state = 'ok' / 'protected' / 'error'
    内容比較 (同一ならコピーしない) より**前**に呼ぶこと。比較のために
    保護対象を開く必要もない。
    """
    try:
        dest, protected = protect.check_dest(HOSTDRV_DIR, guest_path, host_src)
    except protect.ProtectError as exc:
        print("Error: 配備の保護判定に失敗: {}".format(exc), file=sys.stderr)
        return None, 'error'
    if protected:
        protect.protect_log(guest_path)
        return dest, 'protected'
    return dest, 'ok'


def load_deploy_yaml():
    """層ごとの配備定義をマージして返す (tools/deploy_manifests.py に委譲)"""
    return _load_merged()


def resolve_files_from_entry(entry):
    """deploy.yaml の files エントリ1件からホストパスとゲストパスのペアを生成

    Returns: list of (host_abs_path, guest_path)
    """
    host_pattern = entry['host']
    guest = entry['guest']
    entry_type = entry.get('type', 'file')
    exclude = entry.get('exclude', [])

    results = []

    if entry_type == 'glob':
        pattern = os.path.join(PROJ_DIR, host_pattern)
        matched = sorted(globmod.glob(pattern))
        for fpath in matched:
            basename = os.path.basename(fpath)
            if basename in exclude:
                continue
            if not os.path.isfile(fpath):
                continue
            if guest.endswith('/'):
                g = guest + basename
            else:
                g = guest
            results.append((fpath, g))
    else:
        fpath = os.path.join(PROJ_DIR, host_pattern)
        if os.path.isfile(fpath):
            g = guest + os.path.basename(fpath) if guest.endswith('/') else guest
            results.append((fpath, g))
        else:
            print("  Warning: {} not found".format(host_pattern))

    return results


def do_sync(tag_filter=None):
    """deploy.yaml に基づきファイルをHostDrvディレクトリにコピー"""
    cfg = load_deploy_yaml()
    if cfg is None:
        return False

    if not os.path.isdir(HOSTDRV_DIR):
        print("HostDrvディレクトリを作成: {}".format(HOSTDRV_DIR))
        os.makedirs(HOSTDRV_DIR, exist_ok=True)

    print("=" * 55)
    print("  OS32 HostDrv デプロイ")
    print("  {} 層のマニフェスト -> {}".format(len(DEPLOY_MANIFESTS), HOSTDRV_DIR))
    if tag_filter:
        print("  タグフィルタ: {}".format(tag_filter))
    print("=" * 55)

    fs = cfg.get('filesystem', {})

    # ディレクトリ構造作成
    if not tag_filter:
        dirs = fs.get('directories', [])
        for d in dirs:
            target, state = guard_dest(d)
            if state == 'error':
                return False
            if state == 'protected':
                continue
            if not os.path.exists(target):
                os.makedirs(target, exist_ok=True)
                print("  mkdir {}".format(d))

    # ファイルコピー
    files = fs.get('files', [])
    total_copied = 0
    total_skipped = 0
    total_protected = 0
    total_size = 0

    for entry in files:
        entry_tags = entry.get('tags', [])

        if tag_filter and tag_filter not in entry_tags:
            continue

        pairs = resolve_files_from_entry(entry)
        if not pairs:
            continue

        tag_label = entry_tags[0] if entry_tags else 'other'

        for host_abs, guest_path in pairs:
            # 実コピー直前に最終パスで保護判定する。内容比較より前なので、
            # 保護対象は比較のためにすら開かない。
            dest_file, state = guard_dest(guest_path, host_src=host_abs)
            if state == 'error':
                return False
            if state == 'protected':
                total_protected += 1
                continue

            # ゲスト側のディレクトリを確保 (保護対象名のディレクトリは作らない)
            guest_dir = os.path.dirname(
                guest_path if not guest_path.endswith('/')
                else guest_path + os.path.basename(host_abs)) or '/'
            if guest_dir.strip('/'):
                dest_dir, dstate = guard_dest(guest_dir)
                if dstate == 'error':
                    return False
                if dstate == 'protected':
                    total_protected += 1
                    continue
                if not os.path.exists(dest_dir):
                    os.makedirs(dest_dir, exist_ok=True)

            # 同一ファイルならスキップ (サイズ+内容比較)
            if os.path.isfile(dest_file):
                if filecmp.cmp(host_abs, dest_file, shallow=False):
                    total_skipped += 1
                    continue

            # コピー
            shutil.copy2(host_abs, dest_file)
            size = os.path.getsize(host_abs)
            total_size += size
            total_copied += 1
            disp_path = guest_path
            if disp_path.endswith('/'):
                disp_path = disp_path + os.path.basename(host_abs)
            print("  [{}] {} ({} bytes)".format(tag_label, disp_path, size))

    print("")
    print("=" * 55)
    print("  完了! {} ファイル更新 ({:,} bytes), {} スキップ{}".format(
        total_copied, total_size, total_skipped,
        "、{} 件は保護対象として除外".format(total_protected)
        if total_protected else ""))
    print("=" * 55)
    return True


def do_diff(tag_filter=None):
    """ビルド成果物とHostDrvディレクトリの差分を表示"""
    cfg = load_deploy_yaml()
    if cfg is None:
        return

    fs = cfg.get('filesystem', {})
    files = fs.get('files', [])

    changed = 0
    missing = 0
    same = 0

    for entry in files:
        entry_tags = entry.get('tags', [])
        if tag_filter and tag_filter not in entry_tags:
            continue

        pairs = resolve_files_from_entry(entry)
        for host_abs, guest_path in pairs:
            dest_file = os.path.join(HOSTDRV_DIR, guest_path.lstrip('/'))

            if not os.path.isfile(dest_file):
                print("  [NEW]     {}".format(guest_path))
                missing += 1
            elif not filecmp.cmp(host_abs, dest_file, shallow=False):
                src_size = os.path.getsize(host_abs)
                dst_size = os.path.getsize(dest_file)
                print("  [CHANGED] {} ({} -> {} bytes)".format(
                    guest_path, dst_size, src_size))
                changed += 1
            else:
                same += 1

    print("")
    print("変更: {}, 新規: {}, 同一: {}".format(changed, missing, same))


def do_clean():
    """HostDrvディレクトリの中身を削除する (保護対象と、それを含む祖先は残す)

    以前は `shutil.rmtree(root/etc)` で /etc ごと消していた。rmtree は判定の
    余地なく木を落とすので、settings.db の保護をどこに書いても効かない。
    エントリごとに消し、保護対象とその祖先ディレクトリだけ残す (往復 3 の 4)。
    """
    if not os.path.isdir(HOSTDRV_DIR):
        print("HostDrvディレクトリが存在しません: {}".format(HOSTDRV_DIR))
        return True

    kept = 0
    try:
        for dirpath, dirnames, filenames in os.walk(HOSTDRV_DIR, topdown=False):
            for name in filenames:
                path = os.path.join(dirpath, name)
                if protect.is_protected(HOSTDRV_DIR, path):
                    protect.protect_log(protect.guest_path_of(HOSTDRV_DIR, path))
                    kept += 1
                    continue
                os.remove(path)
            for name in dirnames:
                path = os.path.join(dirpath, name)
                if os.path.islink(path):
                    os.remove(path)
                    continue
                if protect.is_protected(HOSTDRV_DIR, path):
                    protect.protect_log(protect.guest_path_of(HOSTDRV_DIR, path))
                    kept += 1
                    continue
                try:
                    os.rmdir(path)
                except OSError:
                    # 保護対象を抱えているので空にならない。祖先として残す。
                    kept += 1
    except protect.ProtectError as exc:
        print("Error: 保護判定に失敗したので clean を中止: {}".format(exc),
              file=sys.stderr)
        return False
    except OSError as exc:
        print("Error: clean に失敗: {}".format(exc), file=sys.stderr)
        return False

    if kept:
        print("クリア完了 (保護対象と祖先 {} 件は残した): {}".format(
            kept, HOSTDRV_DIR))
    else:
        print("クリア完了: {}".format(HOSTDRV_DIR))
    return True


def do_ls(path='/'):
    """HostDrvディレクトリの一覧表示"""
    target = os.path.join(HOSTDRV_DIR, path.lstrip('/'))
    if not os.path.exists(target):
        print("Error: {} not found".format(path), file=sys.stderr)
        return

    if os.path.isfile(target):
        size = os.path.getsize(target)
        print("{} ({} bytes)".format(path, size))
        return

    for item in sorted(os.listdir(target)):
        full = os.path.join(target, item)
        if os.path.isdir(full):
            print("  {}/".format(item))
        else:
            size = os.path.getsize(full)
            print("  {} ({} bytes)".format(item, size))


def main():
    if len(sys.argv) < 2:
        print("HostDrv Deploy Tool")
        print("")
        print("使い方: {} <command>".format(sys.argv[0]))
        print("")
        print("  sync [--tag TAG]  — 層別マニフェストに基づくデプロイ")
        print("  diff [--tag TAG]  — ビルド成果物との差分表示")
        print("  clean             — HostDrvディレクトリをクリア")
        print("  ls [path]         — ファイル一覧")
        print("")
        print("パス:")
        print("  HostDrv:     {}".format(HOSTDRV_DIR))
        print("  マニフェスト: {}".format(", ".join(DEPLOY_MANIFESTS)))
        print("  Project:     {}".format(PROJ_DIR))
        return

    cmd = sys.argv[1]

    if cmd == 'sync':
        tag_filter = None
        i = 2
        while i < len(sys.argv):
            if sys.argv[i] == '--tag' and i + 1 < len(sys.argv):
                tag_filter = sys.argv[i + 1]
                i += 2
            else:
                i += 1
        if not do_sync(tag_filter=tag_filter):
            sys.exit(1)

    elif cmd == 'diff':
        tag_filter = None
        i = 2
        while i < len(sys.argv):
            if sys.argv[i] == '--tag' and i + 1 < len(sys.argv):
                tag_filter = sys.argv[i + 1]
                i += 2
            else:
                i += 1
        do_diff(tag_filter=tag_filter)

    elif cmd == 'clean':
        if not do_clean():
            sys.exit(1)

    elif cmd == 'ls':
        path = sys.argv[2] if len(sys.argv) > 2 else '/'
        do_ls(path)

    else:
        print("Unknown command: {}".format(cmd), file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
