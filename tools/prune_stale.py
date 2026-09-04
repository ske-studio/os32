#!/usr/bin/env python3
"""配備先 (HostDrv / NHD) に残った、マニフェストに無い古いバイナリを一覧・削除する。

配備 (hostdrv_deploy.py / nhd_deploy.py sync) はマニフェストの内容を書くだけで、
マニフェストから外れたファイルを消さない。KAPI のレイアウトが変わると古い
バイナリは別関数へ飛んで rshell ごと沈黙する (CLAUDE.md の Known Gotchas) ので、
配備のたびに掃除する。

対象は **システム側ディレクトリ (/, /bin, /sbin, /usr/bin, /sys, /debug) 直下の
*.bin** だけ。/home /data /etc /tmp などのユーザデータには触れない。

使い方:
    python3 tools/prune_stale.py [hostdrv|nhd|both] [--delete]
      既定は both の一覧表示 (dry-run)。--delete で実際に消す。
      nhd は build/nhd/os32.nhd をマウントして操作する (sudo)。Windows 側へ
      反映するのは deploy-nhd の deploy 段 (NP21/W 停止中のみ)。
終了コード: 0 = 掃除済み or 何も無い、1 = 引数/環境エラー。
一覧の最後に必ず 1 行 `RESULT: ...` を出す (os32-cycle / emu_agent が拾う)。
"""

import os
import sys
import glob
import time
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ_DIR = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import deploy_manifests  # noqa: E402

# 掃除してよいゲスト側ディレクトリ (末尾スラッシュ無し、'' = ルート直下)
PRUNE_DIRS = ('', 'bin', 'sbin', 'usr/bin', 'sys', 'debug')
EXT = '.bin'


def wanted_guest_paths():
    """マニフェスト (core + userland + apps + game) が置くゲストパスの集合"""
    merged = deploy_manifests.load_merged()
    if merged is None:
        return None
    want = set()
    for entry in merged['filesystem']['files']:
        host = entry['host']
        guest = entry['guest']
        pat = os.path.join(PROJ_DIR, host)
        hosts = glob.glob(pat) if any(c in host for c in '*?[') else [pat]
        for hp in hosts:
            base = os.path.basename(hp)
            want.add(guest + base if guest.endswith('/') else guest)
    return want


def find_stale(root, want):
    """root 配下の PRUNE_DIRS 直下にある *.bin でマニフェストに無いもの"""
    stale = []
    for d in PRUNE_DIRS:
        dp = os.path.join(root, d) if d else root
        if not os.path.isdir(dp):
            continue
        for f in sorted(os.listdir(dp)):
            if not f.endswith(EXT):
                continue
            p = os.path.join(dp, f)
            if not os.path.isfile(p):
                continue
            gp = '/' + (d + '/' if d else '') + f
            if gp not in want:
                stale.append((gp, p))
    return stale


def show(label, stale):
    print("[{}] マニフェストに無い {} : {} 件".format(label, EXT, len(stale)))
    for gp, p in stale:
        try:
            st = os.stat(p)
            print("  {:<36} {:>9}  {}".format(
                gp, st.st_size, time.strftime('%Y-%m-%d', time.localtime(st.st_mtime))))
        except OSError:
            print("  {:<36} (stat 失敗)".format(gp))


def hostdrv_root():
    if os.environ.get('HOSTDRV_DIR'):
        return os.environ['HOSTDRV_DIR']
    env_path = os.path.join(PROJ_DIR, '.env')
    if os.path.isfile(env_path):
        with open(env_path) as f:
            for line in f:
                line = line.strip()
                if line.startswith('HOSTDRV_DIR='):
                    return line.split('=', 1)[1].strip().strip('"').strip("'")
    return '/mnt/c/os32'


def prune_hostdrv(want, delete):
    root = hostdrv_root()
    if not os.path.isdir(root):
        print("Error: HOSTDRV_DIR {} が無い".format(root), file=sys.stderr)
        return None
    stale = find_stale(root, want)
    show('hostdrv ' + root, stale)
    if delete:
        for gp, p in stale:
            os.remove(p)
            print("  removed {}".format(gp))
    return len(stale)


def prune_nhd(want, delete):
    import nhd_deploy  # sudo mount まわりを流用
    if not nhd_deploy.ensure_local_nhd():
        return None
    if not nhd_deploy.ensure_mounted():
        print("Error: NHD をマウントできない", file=sys.stderr)
        return None
    root = nhd_deploy.MOUNT_POINT
    stale = find_stale(root, want)
    show('nhd ' + nhd_deploy.NHD_LOCAL, stale)
    if delete:
        for gp, p in stale:
            subprocess.run(['sudo', 'rm', '-f', p], capture_output=True)
            print("  removed {}".format(gp))
        subprocess.run(['sync'], capture_output=True)
        print("  (Windows 側への反映は deploy-nhd の deploy 段。NP21/W 停止中に行うこと)")
    return len(stale)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    delete = '--delete' in sys.argv[1:]
    target = args[0] if args else 'both'
    if target not in ('hostdrv', 'nhd', 'both'):
        print(__doc__)
        return 1
    want = wanted_guest_paths()
    if want is None:
        print("RESULT: FAIL 配備マニフェストが読めない")
        return 1
    counts = {}
    if target in ('hostdrv', 'both'):
        counts['hostdrv'] = prune_hostdrv(want, delete)
    if target in ('nhd', 'both'):
        counts['nhd'] = prune_nhd(want, delete)
    if any(v is None for v in counts.values()):
        print("RESULT: FAIL " + ", ".join(
            "{}={}".format(k, 'error' if v is None else v) for k, v in counts.items()))
        return 1
    verb = '削除' if delete else '検出 (未削除、--delete で消す)'
    print("RESULT: OK " + ", ".join("{}={}".format(k, v) for k, v in counts.items())
          + " 件を" + verb)
    return 0


if __name__ == '__main__':
    sys.exit(main())
