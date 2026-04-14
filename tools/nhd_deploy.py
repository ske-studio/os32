#!/usr/bin/env python3
"""
nhd_deploy.py — NHDイメージのext2パーティションをmountして操作する

/tmp/os32.nhd をループデバイスでマウントし、通常のファイル操作でデプロイする。
NP21/Wへの反映は deploy コマンドで /tmp/os32.nhd をWindows側にコピーする。

前提:
  sudoers に以下が設定済み (NOPASSWD):
    /usr/bin/mount, /usr/bin/umount, /usr/sbin/losetup,
    /usr/sbin/e2fsck, /usr/sbin/mkfs.ext2, /usr/sbin/mke2fs

使い方:
  python3 nhd_deploy.py mount              — ext2パーティションをマウント
  python3 nhd_deploy.py umount             — アンマウント
  python3 nhd_deploy.py copy <src> [...]   — ファイルをext2にコピー
  python3 nhd_deploy.py copy-all <dir>     — dirの全.binをコピー
  python3 nhd_deploy.py ls [path]          — ファイル一覧
  python3 nhd_deploy.py rm <file>          — ファイル削除
  python3 nhd_deploy.py deploy             — umount + NHDをNP21/Wにコピー
  python3 nhd_deploy.py write-kernel <k> [l] — カーネルをブート領域に書き込み
  python3 nhd_deploy.py format             — ext2を再フォーマット (データ全消去)
  python3 nhd_deploy.py init               — Windows側NHDを/tmpにコピー+フォーマット+マウント
"""

import sys
import os
import subprocess
import shutil
import glob as globmod

# === パス設定 ===
NHD_LOCAL = "/tmp/os32.nhd"
NHD_REMOTE = r"/mnt/c/Users/hight/OneDrive/ドキュメント/np21w/os32.nhd"
MOUNT_POINT = "/tmp/os32"

# === ext2パーティション オフセット ===
# NHDヘッダ(512B) + ブート領域(LBA 0-271) = 273セクタ
NHD_HEADER_SECTORS = 1
HDD_PARTITION_LBA = 272
PARTITION_SKIP = NHD_HEADER_SECTORS + HDD_PARTITION_LBA  # 273
PARTITION_OFFSET = PARTITION_SKIP * 512  # 139776 バイト


def is_mounted():
    """マウント済みかチェック"""
    result = subprocess.run(
        ['mountpoint', '-q', MOUNT_POINT],
        capture_output=True
    )
    return result.returncode == 0


def get_loop_device():
    """現在NHD_LOCALに紐づいているループデバイスを返す (なければNone)"""
    result = subprocess.run(
        ['losetup', '-j', NHD_LOCAL],
        capture_output=True, text=True
    )
    if result.returncode == 0 and result.stdout.strip():
        # "/dev/loop0: ..." のような出力
        return result.stdout.strip().split(':')[0]
    return None


def do_mount():
    """ext2パーティションをマウント"""
    if is_mounted():
        print("既にマウント済みです: " + MOUNT_POINT)
        return True

    if not os.path.isfile(NHD_LOCAL):
        print("Error: {} が見つかりません".format(NHD_LOCAL), file=sys.stderr)
        print("  'init' コマンドでWindows側からコピーしてください", file=sys.stderr)
        return False

    # マウントポイント作成
    os.makedirs(MOUNT_POINT, exist_ok=True)

    # ループデバイス作成
    loop_dev = get_loop_device()
    if not loop_dev:
        result = subprocess.run(
            ['sudo', 'losetup', '-f', '--show',
             '--offset', str(PARTITION_OFFSET), NHD_LOCAL],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print("Error: losetup 失敗: " + result.stderr.strip(),
                  file=sys.stderr)
            return False
        loop_dev = result.stdout.strip()

    # マウント
    result = subprocess.run(
        ['sudo', 'mount', '-t', 'ext2', loop_dev, MOUNT_POINT],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print("Error: mount 失敗: " + result.stderr.strip(), file=sys.stderr)
        # ループデバイスを解放
        subprocess.run(['sudo', 'losetup', '-d', loop_dev],
                       capture_output=True)
        return False



    print("マウント完了: {} -> {}".format(loop_dev, MOUNT_POINT))
    return True


def do_umount():
    """アンマウント"""
    if not is_mounted():
        print("マウントされていません")
        # ループデバイスが残っていたら解放
        loop_dev = get_loop_device()
        if loop_dev:
            subprocess.run(['sudo', 'losetup', '-d', loop_dev],
                           capture_output=True)
            print("ループデバイス {} を解放しました".format(loop_dev))
        return True

    # sync
    subprocess.run(['sync'], capture_output=True)

    # アンマウント
    result = subprocess.run(
        ['sudo', 'umount', MOUNT_POINT],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print("Error: umount 失敗: " + result.stderr.strip(),
              file=sys.stderr)
        return False

    # ループデバイス解放
    loop_dev = get_loop_device()
    if loop_dev:
        subprocess.run(['sudo', 'losetup', '-d', loop_dev],
                       capture_output=True)

    print("アンマウント完了")
    return True


def ensure_mounted():
    """マウントされていなければ自動マウントする"""
    if is_mounted():
        return True
    print("自動マウント中...")
    return do_mount()


# === ディレクトリ構造定義 ===
SYS_DIRS = ['bin', 'sbin', 'usr', 'usr/bin', 'usr/man', 'data', 'etc',
            'home', 'home/user', 'tmp']


def do_mkdirs():
    """ext2上にシステムディレクトリを作成"""
    if not ensure_mounted():
        return False
    for d in SYS_DIRS:
        target = os.path.join(MOUNT_POINT, d)
        if not os.path.exists(target):
            subprocess.run(['sudo', 'mkdir', '-p', target],
                           capture_output=True)
            print("  mkdir /{}".format(d))
    subprocess.run(['sync'], capture_output=True)
    print("Done! (system directories created)")
    return True


def do_copy(src_files, dest_dir='/', rename=None):
    """ファイルをマウント済みext2にコピー (sudo cp)

    dest_dir: コピー先ディレクトリ (例: '/bin', '/usr/bin')
    rename:   ファイル名を変更 (単一ファイルのみ有効)
    """
    if not ensure_mounted():
        return False

    # コピー先ディレクトリの確保
    dest_base = os.path.join(MOUNT_POINT, dest_dir.lstrip('/'))
    if not os.path.exists(dest_base):
        subprocess.run(['sudo', 'mkdir', '-p', dest_base],
                       capture_output=True)

    copied = 0
    for src in src_files:
        if not os.path.isfile(src):
            print("Warning: {} not found, skipping".format(src))
            continue
        if rename and len(src_files) == 1:
            dest_name = rename
        else:
            dest_name = os.path.basename(src)
        dest_path = os.path.join(dest_base, dest_name)
        result = subprocess.run(
            ['sudo', 'cp', src, dest_path],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print("Error copying {}: {}".format(dest_name, result.stderr.strip()),
                  file=sys.stderr)
            continue
        size = os.path.getsize(src)
        disp_dir = dest_dir if dest_dir.endswith('/') else dest_dir + '/'
        print("  {}{} ({} bytes)".format(disp_dir, dest_name, size))
        copied += 1

    if copied > 0:
        subprocess.run(['sync'], capture_output=True)
        print("Done! ({} files copied)".format(copied))
    else:
        print("Error: コピーするファイルがありません", file=sys.stderr)
        return False
    return True


def do_copy_all(src_dir, ext='.bin', dest_dir='/'):
    """ディレクトリ内の全ファイルをコピー"""
    pattern = os.path.join(src_dir, '*{}'.format(ext))
    files = sorted(globmod.glob(pattern))
    if not files:
        print("No {} files found in {}".format(ext, src_dir))
        return False
    print("=== Batch copy: {} files from {} to {} ===".format(
        len(files), src_dir, dest_dir))
    return do_copy(files, dest_dir=dest_dir)


def do_ls(path='/'):
    """ファイル一覧"""
    if not ensure_mounted():
        return
    target = os.path.join(MOUNT_POINT, path.lstrip('/'))
    if not os.path.exists(target):
        print("Error: {} not found".format(path), file=sys.stderr)
        return
    result = subprocess.run(
        ['ls', '-la', target],
        capture_output=True, text=True
    )
    print(result.stdout)


def do_rm(filename):
    """ファイル削除 (sudo rm)"""
    if not ensure_mounted():
        return
    target = os.path.join(MOUNT_POINT, filename.lstrip('/'))
    if not os.path.exists(target):
        print("Error: {} not found".format(filename), file=sys.stderr)
        return
    subprocess.run(['sudo', 'rm', target], capture_output=True)
    subprocess.run(['sync'], capture_output=True)
    print("Removed: {}".format(filename))


def do_deploy():
    """アンマウント + NHDをNP21/Wにコピー"""
    # まずアンマウント
    if is_mounted():
        if not do_umount():
            return False

    if not os.path.isfile(NHD_LOCAL):
        print("Error: {} が見つかりません".format(NHD_LOCAL), file=sys.stderr)
        return False

    print("NHDイメージをNP21/Wにコピー中...")
    print("  {} -> {}".format(NHD_LOCAL, NHD_REMOTE))

    try:
        shutil.copy2(NHD_LOCAL, NHD_REMOTE)
    except PermissionError:
        print("Error: NP21/Wがファイルをロックしています。先にkillしてください",
              file=sys.stderr)
        print("  taskkill.exe /F /IM np21x64w.exe", file=sys.stderr)
        return False

    size_mb = os.path.getsize(NHD_LOCAL) / (1024 * 1024)
    print("Done! ({:.1f} MB copied)".format(size_mb))
    return True


def do_write_kernel(kernel_bin, loader_bin=None):
    """NHDのブート領域にloader+kernelを直接書き込む

    NHDレイアウト (512B/セクタ):
      NHDヘッダ: 512B (オフセット0)
      LBA 0: IPL (boot_hdd.bin)
      LBA 1: パーティションテーブル
      LBA 2-5: loader_hdd.bin (最大4セクタ = 2048B)
      LBA 6+: kernel.bin
      LBA 272+: ext2パーティション
    """
    NHD_HEADER = 512
    SECTOR = 512
    KERNEL_LBA = 6
    LOADER_LBA = 2

    kernel_offset = NHD_HEADER + KERNEL_LBA * SECTOR
    loader_offset = NHD_HEADER + LOADER_LBA * SECTOR

    with open(kernel_bin, 'rb') as f:
        kernel_data = f.read()

    print("  kernel.bin: {} bytes ({} sectors)".format(
        len(kernel_data), (len(kernel_data) + 511) // 512))

    with open(NHD_LOCAL, 'r+b') as nhd:
        # loader書き込み
        if loader_bin and os.path.isfile(loader_bin):
            with open(loader_bin, 'rb') as f:
                loader_data = f.read()
            nhd.seek(loader_offset)
            nhd.write(loader_data)
            print("  loader:     {} bytes -> LBA {}".format(
                len(loader_data), LOADER_LBA))

        # kernel書き込み
        nhd.seek(kernel_offset)
        nhd.write(kernel_data)
        print("  kernel:     {} bytes -> LBA {}".format(
            len(kernel_data), KERNEL_LBA))

    print("Done!")


def do_format():
    """ext2パーティションを再フォーマット (データ全消去)"""
    if is_mounted():
        print("マウント中のためアンマウントします...")
        if not do_umount():
            return False

    if not os.path.isfile(NHD_LOCAL):
        print("Error: {} が見つかりません".format(NHD_LOCAL), file=sys.stderr)
        return False

    # ループデバイス作成
    result = subprocess.run(
        ['sudo', 'losetup', '-f', '--show',
         '--offset', str(PARTITION_OFFSET), NHD_LOCAL],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print("Error: losetup 失敗", file=sys.stderr)
        return False
    loop_dev = result.stdout.strip()

    print("ext2をフォーマット中... ({})".format(loop_dev))
    result = subprocess.run(
        ['sudo', 'mkfs.ext2', '-b', '1024', '-I', '128',
         '-L', 'OS32_HDD', '-F', loop_dev],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print("Error: mkfs.ext2 失敗: " + result.stderr.strip(),
              file=sys.stderr)
    else:
        print("フォーマット完了!")
        print(result.stdout)

    # ループデバイス解放
    subprocess.run(['sudo', 'losetup', '-d', loop_dev], capture_output=True)
    return result.returncode == 0


def do_init():
    """Windows側NHDを/tmpにコピー + フォーマット + マウント"""
    if is_mounted():
        print("既にマウント済みです。先にumountしてください。")
        return False

    if os.path.isfile(NHD_LOCAL):
        print("{} は既に存在します。上書きします...".format(NHD_LOCAL))

    if not os.path.isfile(NHD_REMOTE):
        print("Error: {} が見つかりません".format(NHD_REMOTE),
              file=sys.stderr)
        return False

    print("NHDイメージをコピー中...")
    print("  {} -> {}".format(NHD_REMOTE, NHD_LOCAL))

    try:
        shutil.copy2(NHD_REMOTE, NHD_LOCAL)
    except PermissionError:
        print("Error: NP21/Wがファイルをロックしています",
              file=sys.stderr)
        return False

    size_mb = os.path.getsize(NHD_LOCAL) / (1024 * 1024)
    print("コピー完了! ({:.1f} MB)".format(size_mb))

    # フォーマット
    print("")
    if not do_format():
        return False

    # マウント
    print("")
    return do_mount()


def main():
    if len(sys.argv) < 2:
        print("NHD ext2 Deploy Tool (mount版)")
        print("")
        print("使い方: {} <command>".format(sys.argv[0]))
        print("")
        print("  mount                  — ext2パーティションをマウント")
        print("  umount                 — アンマウント")
        print("  copy [--dest DIR] [--rename NAME] <src> [...] — ファイルをext2にコピー")
        print("  copy-all [--dest DIR] <dir> [ext]   — dirの全ファイルを一括コピー")
        print("  setup-dirs             — システムディレクトリを作成")
        print("  ls [path]              — ファイル一覧")
        print("  rm <file>              — ファイル削除")
        print("  deploy                 — umount + NHDをNP21/Wにコピー")
        print("  write-kernel <k> [ldr] — カーネルをブート領域に書き込み")
        print("  format                 — ext2を再フォーマット (全消去)")
        print("  init                   — Windows側NHDをコピー+フォーマット+マウント")
        print("")
        print("パス:")
        print("  NHDローカル:  {}".format(NHD_LOCAL))
        print("  NHD NP21/W:   {}".format(NHD_REMOTE))
        print("  マウント:     {}".format(MOUNT_POINT))
        return

    cmd = sys.argv[1]

    if cmd == 'mount':
        do_mount()

    elif cmd == 'umount':
        do_umount()

    elif cmd == 'setup-dirs':
        do_mkdirs()

    elif cmd == 'copy':
        # --dest DIR と --rename NAME オプションをパース
        dest_dir = '/'
        rename = None
        src_files = []
        i = 2
        while i < len(sys.argv):
            if sys.argv[i] == '--dest' and i + 1 < len(sys.argv):
                dest_dir = sys.argv[i + 1]
                i += 2
            elif sys.argv[i] == '--rename' and i + 1 < len(sys.argv):
                rename = sys.argv[i + 1]
                i += 2
            else:
                src_files.append(sys.argv[i])
                i += 1
        if not src_files:
            print("Usage: copy [--dest DIR] [--rename NAME] <src_file> [...]")
            return
        do_copy(src_files, dest_dir=dest_dir, rename=rename)

    elif cmd == 'copy-all':
        # --dest DIR オプションをパース
        dest_dir = '/'
        args = []
        i = 2
        while i < len(sys.argv):
            if sys.argv[i] == '--dest' and i + 1 < len(sys.argv):
                dest_dir = sys.argv[i + 1]
                i += 2
            else:
                args.append(sys.argv[i])
                i += 1
        if not args:
            print("Usage: copy-all [--dest DIR] <dir> [extension]")
            return
        src_dir = args[0]
        ext = args[1] if len(args) > 1 else '.bin'
        do_copy_all(src_dir, ext, dest_dir=dest_dir)

    elif cmd == 'ls':
        path = sys.argv[2] if len(sys.argv) > 2 else '/'
        do_ls(path)

    elif cmd == 'rm':
        if len(sys.argv) < 3:
            print("Usage: rm <file>")
            return
        do_rm(sys.argv[2])

    elif cmd == 'deploy':
        do_deploy()

    elif cmd == 'write-kernel':
        if len(sys.argv) < 3:
            print("Usage: write-kernel <kernel.bin> [loader_hdd.bin]")
            return
        kernel = sys.argv[2]
        loader = sys.argv[3] if len(sys.argv) > 3 else None
        if not os.path.isfile(kernel):
            print("Error: {} not found".format(kernel))
            return
        # write-kernelはマウント不要 (ブート領域への直接書き込み)
        do_write_kernel(kernel, loader)

    elif cmd == 'format':
        do_format()

    elif cmd == 'init':
        do_init()

    else:
        print("Unknown command: {}".format(cmd))


if __name__ == '__main__':
    main()
