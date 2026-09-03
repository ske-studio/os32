#!/usr/bin/env python3
"""
nhd_deploy.py — NHDイメージのext2パーティションをmountして操作する

build/nhd/os32.nhd (NHD_LOCAL) をループデバイスでマウントし、通常のファイル操作で
デプロイする。NP21/Wへの反映は deploy コマンドでそれをWindows側にコピーする。
NHD_LOCAL が無ければ Windows 側から自動で取り込む (NP21/W 停止中のみ可能)。

前提:
  sudoers に以下が設定済み (NOPASSWD):
    /usr/bin/mount, /usr/bin/umount, /usr/sbin/losetup,
    /usr/sbin/e2fsck, /usr/sbin/mkfs.ext2, /usr/sbin/mke2fs

使い方:
  python3 nhd_deploy.py sync [--tag TAG]   — deploy.yaml に基づくフルデプロイ
  python3 nhd_deploy.py mount              — ext2パーティションをマウント
  python3 nhd_deploy.py umount             — アンマウント
  python3 nhd_deploy.py copy <src> [...]   — ファイルをext2にコピー
  python3 nhd_deploy.py copy-all <dir>     — dirの全.binをコピー
  python3 nhd_deploy.py ls [path]          — ファイル一覧
  python3 nhd_deploy.py rm <file>          — ファイル削除
  python3 nhd_deploy.py deploy             — umount + NHDをNP21/Wにコピー
  python3 nhd_deploy.py format             — ext2を再フォーマット (データ全消去)
  python3 nhd_deploy.py init               — Windows側NHDを/tmpにコピー+フォーマット+マウント
"""

import sys
import os
import subprocess
import shutil
import glob as globmod
import yaml

# === パス設定 ===
# 作業用 NHD (ループマウントして書き込む側)。以前は /tmp/os32.nhd だったが、
# /tmp は WSL 再起動で消え、消えた状態で make deploy-nhd を走らせると何も
# 書かずに成功扱いになっていた (2026-09-04)。リポジトリ内の build/nhd/ に置く
# (gitignore 済み、make clean の対象外)。NHD_LOCAL は PROJ_DIR の後で決める。

# NP21W_DIR: 環境変数 → .env → .env.sample → デフォルト の順で解決
def _resolve_np21w_dir():
    """NP21W_DIRを.envから解決する"""
    if os.environ.get('NP21W_DIR'):
        return os.environ['NP21W_DIR']
    proj_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for env_file in ['.env', '.env.sample']:
        env_path = os.path.join(proj_dir, env_file)
        if os.path.isfile(env_path):
            with open(env_path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if line.startswith('NP21W_DIR='):
                        return line.split('=', 1)[1].strip()
    return "/tmp/np21w"

NP21W_DIR = _resolve_np21w_dir()
NHD_REMOTE = os.path.join(NP21W_DIR, "os32.nhd")
MOUNT_POINT = "/tmp/os32"

# プロジェクトルート (tools/ の親ディレクトリ)
PROJ_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NHD_LOCAL = os.environ.get("OS32_NHD_LOCAL") or os.path.join(PROJ_DIR, "build", "nhd", "os32.nhd")
# 配備定義は所有する層ごとに分かれている。リストとマージ処理の実体は
# tools/deploy_manifests.py。二重に持つと食い違うので参照だけにすること。
# 各マニフェストは自層の成果物しか参照しない (make check-manifests で検査)。
from deploy_manifests import DEPLOY_MANIFESTS, load_merged as _load_merged

# === ext2パーティション オフセット ===
# NHDヘッダ(512B) + ブート領域(LBA 0-1631) = 1633セクタ
# シリンダ境界整列: 8H x 17SPT = 136sec/cyl, シリンダ12 = LBA 1632
# (-O0 SQLiteバイナリが最大LBA 1406まで使用するため拡張)
NHD_HEADER_SECTORS = 1
HDD_PARTITION_LBA = 1632
PARTITION_SKIP = NHD_HEADER_SECTORS + HDD_PARTITION_LBA  # 1633
PARTITION_OFFSET = PARTITION_SKIP * 512  # 836096 バイト (1633 * 512)


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



def ensure_local_nhd():
    """NHD_LOCAL が無ければ Windows 側から取り込む (NP21/W 停止中のみ可能)。
    build/nhd/ は make clean の対象外だが、clone 直後や手で消した後は無い。"""
    if os.path.isfile(NHD_LOCAL):
        return True
    print("{} が無いので Windows 側 NHD から取り込みます".format(NHD_LOCAL))
    if not os.path.isfile(NHD_REMOTE):
        print("Error: {} も見つかりません".format(NHD_REMOTE), file=sys.stderr)
        return False
    os.makedirs(os.path.dirname(NHD_LOCAL), exist_ok=True)
    try:
        shutil.copy2(NHD_REMOTE, NHD_LOCAL)
    except PermissionError:
        print("Error: NP21/Wがファイルをロックしています。先にkillしてください",
              file=sys.stderr)
        print("  taskkill.exe /F /IM np21x64w.exe", file=sys.stderr)
        return False
    print("  取り込み完了 ({:.1f} MB)".format(os.path.getsize(NHD_LOCAL) / (1024 * 1024)))
    return True


def do_mount():
    """ext2パーティションをマウント"""
    if is_mounted():
        print("既にマウント済みです: " + MOUNT_POINT)
        return True

    if not ensure_local_nhd():
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

    if not ensure_local_nhd():
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


def do_write_boot(loader_bin):
    """NHDのブート領域にローダーのみを書き込む (新方式)

    カーネル/SQLiteはext2のvmkernel.lz4として配置されるため、
    rawセクタ書き込みはローダー (LBA 2-17) のみ。
    """
    NHD_HEADER = 512
    SECTOR = 512
    LOADER_LBA = 2
    MAX_LOADER_SECTORS = 16  # 8KB

    loader_offset = NHD_HEADER + LOADER_LBA * SECTOR

    with open(loader_bin, 'rb') as f:
        loader_data = f.read()

    if len(loader_data) > MAX_LOADER_SECTORS * SECTOR:
        print("Error: ローダーが{}Bを超過 ({} bytes)".format(
            MAX_LOADER_SECTORS * SECTOR, len(loader_data)))
        return False

    # 8KBにパディング
    loader_data = loader_data.ljust(MAX_LOADER_SECTORS * SECTOR, b'\x00')

    # /tmp は再起動で消える。素の FileNotFoundError を投げると原因が
    # 分からないので、do_mount と同じ案内を出して False を返す。
    if not os.path.isfile(NHD_LOCAL):
        print("Error: {} が見つかりません".format(NHD_LOCAL), file=sys.stderr)
        print("  'python3 tools/nhd_deploy.py pull' で取り込めます",
              file=sys.stderr)
        print("  (make nhd-init はフォーマットを伴うので通常は pull を使う)",
              file=sys.stderr)
        return False

    with open(NHD_LOCAL, 'r+b') as nhd:
        nhd.seek(loader_offset)
        nhd.write(loader_data)

    print("  loader: {} bytes -> LBA {}-{}".format(
        len(loader_data), LOADER_LBA, LOADER_LBA + MAX_LOADER_SECTORS - 1))
    print("Done!")
    return True


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


def update_partition_table(nhd_path):
    """パーティションテーブル(LBA 1)のCHS開始位置を更新する

    PC-98パーティションテーブルエントリ (32バイト):
      offset 0: bootable flag (0x80=active)
      offset 1: system type (0xE2=OS32)
      offset 6: start sector
      offset 7: start head
      offset 8-9: start cylinder (little-endian)

    ジオメトリ: 8 heads, 17 sectors/track
    """
    NHD_HEADER = 512
    PT_OFFSET = NHD_HEADER + 512  # LBA 1

    # シリンダ番号を計算 (HDD_PARTITION_LBA / (heads * spt))
    heads = 8
    spt = 17
    start_cyl = HDD_PARTITION_LBA // (heads * spt)

    with open(nhd_path, 'r+b') as f:
        f.seek(PT_OFFSET)
        pt = bytearray(f.read(512))

        # Entry 0のCHS開始位置を更新
        pt[6] = 0                                    # start sector = 0
        pt[7] = 0                                    # start head = 0
        pt[8] = start_cyl & 0xFF                     # start cylinder low
        pt[9] = (start_cyl >> 8) & 0xFF              # start cylinder high

        f.seek(PT_OFFSET)
        f.write(pt)

    print("パーティションテーブル更新: 開始シリンダ={} (LBA {})".format(
        start_cyl, HDD_PARTITION_LBA))


def do_pull():
    """Windows側NHDを/tmpに取り込む (フォーマットしない)

    /tmp は再起動で消えるため NHD_LOCAL は頻繁に失われる。init は
    フォーマットを伴いゲスト側で作られたデータ (home/db/save) を消すので、
    作業を再開したいだけのときはこちらを使う。
    """
    if is_mounted():
        print("マウント中です。先に umount してください。", file=sys.stderr)
        return False

    if not os.path.isfile(NHD_REMOTE):
        print("Error: {} が見つかりません".format(NHD_REMOTE), file=sys.stderr)
        return False

    print("NHDイメージを取り込み中 (フォーマットなし)...")
    print("  {} -> {}".format(NHD_REMOTE, NHD_LOCAL))
    os.makedirs(os.path.dirname(NHD_LOCAL), exist_ok=True)
    try:
        shutil.copy2(NHD_REMOTE, NHD_LOCAL)
    except PermissionError:
        print("Error: NP21/Wがファイルをロックしています。先にkillしてください",
              file=sys.stderr)
        print("  taskkill.exe /F /IM np21x64w.exe", file=sys.stderr)
        return False

    print("完了! ({:.1f} MB)".format(os.path.getsize(NHD_LOCAL) / (1024 * 1024)))
    return do_mount()


def do_init():
    """Windows側NHDを/tmpにコピー + パーティション更新 + フォーマット + マウント"""
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

    os.makedirs(os.path.dirname(NHD_LOCAL), exist_ok=True)
    try:
        shutil.copy2(NHD_REMOTE, NHD_LOCAL)
    except PermissionError:
        print("Error: NP21/Wがファイルをロックしています",
              file=sys.stderr)
        return False

    size_mb = os.path.getsize(NHD_LOCAL) / (1024 * 1024)
    print("コピー完了! ({:.1f} MB)".format(size_mb))

    # パーティションテーブル更新 (CHS開始位置をシリンダ8に)
    print("")
    update_partition_table(NHD_LOCAL)

    # フォーマット
    print("")
    if not do_format():
        return False

    # マウント
    print("")
    return do_mount()


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
            # ゲストパスがディレクトリ ('/bin/' 等) ならファイル名を追加
            if guest.endswith('/'):
                g = guest + basename
            else:
                g = guest
            results.append((fpath, g))
    else:
        fpath = os.path.join(PROJ_DIR, host_pattern)
        if os.path.isfile(fpath):
            results.append((fpath, guest))
        else:
            print("  Warning: {} not found".format(host_pattern))

    return results


def do_sync(tag_filter=None):
    """deploy.yaml に基づくフルデプロイ

    1. ブートローダー書き込み (write-boot)
    2. ディレクトリ構造作成
    3. 全ファイルコピー
    """
    if not ensure_local_nhd():
        return False
    cfg = load_deploy_yaml()
    if cfg is None:
        return False

    print("=" * 55)
    print("  OS32 フルデプロイ (deploy.yaml)")
    if tag_filter:
        print("  タグフィルタ: {}".format(tag_filter))
    print("=" * 55)

    # === Phase 1: ブート領域 (ローダーのみrawセクタ書き込み) ===
    if not tag_filter:
        boot = cfg.get('boot', {})
        loader_path = os.path.join(PROJ_DIR, boot.get('loader', ''))

        if loader_path and os.path.isfile(loader_path):
            print("\n[boot] ローダー書き込み")
            if not do_write_boot(loader_path):
                return False
        else:
            print("Warning: ローダー {} が見つかりません".format(loader_path))

    # === Phase 2: ext2 マウント + ディレクトリ作成 ===
    fs = cfg.get('filesystem', {})

    if not ensure_mounted():
        return False

    if not tag_filter:
        dirs = fs.get('directories', [])
        print("\n[dirs] ディレクトリ構造作成")
        for d in dirs:
            target = os.path.join(MOUNT_POINT, d.lstrip('/'))
            if not os.path.exists(target):
                subprocess.run(['sudo', 'mkdir', '-p', target],
                               capture_output=True)
                print("  mkdir {}".format(d))

    # === Phase 3: ファイルコピー ===
    files = fs.get('files', [])
    total_copied = 0
    total_size = 0

    for entry in files:
        entry_tags = entry.get('tags', [])

        # タグフィルタ
        if tag_filter and tag_filter not in entry_tags:
            continue

        pairs = resolve_files_from_entry(entry)
        if not pairs:
            continue

        tag_label = entry_tags[0] if entry_tags else 'other'
        print("\n[{}] {} -> {}".format(tag_label, entry['host'], entry['guest']))

        for host_abs, guest_path in pairs:
            # ゲスト側のディレクトリを確保
            guest_dir = os.path.dirname(guest_path)
            dest_dir_abs = os.path.join(MOUNT_POINT, guest_dir.lstrip('/'))
            if not os.path.exists(dest_dir_abs):
                subprocess.run(['sudo', 'mkdir', '-p', dest_dir_abs],
                               capture_output=True)

            # ファイルコピー
            dest_file = os.path.join(MOUNT_POINT, guest_path.lstrip('/'))
            result = subprocess.run(
                ['sudo', 'cp', host_abs, dest_file],
                capture_output=True, text=True
            )
            if result.returncode != 0:
                print("  Error: {} -> {}: {}".format(
                    os.path.basename(host_abs), guest_path,
                    result.stderr.strip()))
                continue

            size = os.path.getsize(host_abs)
            total_size += size
            total_copied += 1
            print("  {} ({} bytes)".format(guest_path, size))

    # sync
    subprocess.run(['sync'], capture_output=True)

    print("\n" + "=" * 55)
    print("  完了! {} ファイル ({:,} bytes)".format(total_copied, total_size))
    print("=" * 55)
    return True


def do_sync_from_hostdrv():
    """HostDrvディレクトリ (C:\\os32) の内容をNHDのext2パーティションに同期

    deploy.yaml を参照せず、HostDrvディレクトリの全ファイルを再帰的にコピーする。
    これにより HostDrv が唯一のソースとなり、管理漏れを防止する。
    """
    hostdrv_dir = os.environ.get('HOSTDRV_DIR', '/mnt/c/os32')

    if not os.path.isdir(hostdrv_dir):
        print("Error: HostDrvディレクトリが見つかりません: {}".format(hostdrv_dir),
              file=sys.stderr)
        return False

    if not ensure_mounted():
        return False

    print("\n" + "=" * 55)
    print("  HostDrv -> NHD ext2 同期")
    print("  {} -> {}".format(hostdrv_dir, MOUNT_POINT))
    print("=" * 55)

    total_copied = 0
    total_size = 0

    for dirpath, dirnames, filenames in os.walk(hostdrv_dir):
        # HostDrvルートからの相対パス
        rel_dir = os.path.relpath(dirpath, hostdrv_dir)
        if rel_dir == '.':
            rel_dir = ''

        # NHD側のディレクトリを確保
        dest_dir = os.path.join(MOUNT_POINT, rel_dir)
        if not os.path.exists(dest_dir):
            subprocess.run(['sudo', 'mkdir', '-p', dest_dir],
                           capture_output=True)

        for fname in sorted(filenames):
            src_path = os.path.join(dirpath, fname)
            if rel_dir:
                guest_path = '/' + rel_dir + '/' + fname
            else:
                guest_path = '/' + fname
            dest_path = os.path.join(dest_dir, fname)

            result = subprocess.run(
                ['sudo', 'cp', src_path, dest_path],
                capture_output=True, text=True
            )
            if result.returncode != 0:
                print("  Error: {} -> {}: {}".format(
                    fname, guest_path, result.stderr.strip()))
                continue

            size = os.path.getsize(src_path)
            total_size += size
            total_copied += 1
            print("  {} ({} bytes)".format(guest_path, size))

    subprocess.run(['sync'], capture_output=True)

    print("\n" + "=" * 55)
    print("  完了! {} ファイル ({:,} bytes)".format(total_copied, total_size))
    print("=" * 55)
    return True


def resolve_guest_path(host_file):
    """マニフェストからホストファイルに対応するゲストパスを解決する

    照合はプロジェクト相対のフルパスで行う。以前は glob パターンの
    ベース名 (``*.bin``) だけを見ていたため、どの .bin も最初に現れた
    ``*.bin`` エントリに吸い込まれ、apps/ のアプリが /bin/ に解決されていた。

    優先順:
      1. type: file の host 完全一致
      2. type: glob のパターン一致 (フルパス)
      3. 互換のためのベース名一致

    Returns: ゲストパス文字列 (見つからなければ None)
    """
    import fnmatch

    cfg = load_deploy_yaml()
    if cfg is None:
        return None

    relpath = os.path.relpath(os.path.abspath(host_file), PROJ_DIR)
    relpath = relpath.replace(os.sep, '/')
    basename = os.path.basename(host_file)
    files = (cfg.get('filesystem') or {}).get('files') or []

    def _guest_for(entry):
        guest = entry['guest']
        if basename in (entry.get('exclude') or []):
            return None
        return guest + basename if guest.endswith('/') else guest

    # 1. 完全一致
    for entry in files:
        if entry.get('type', 'file') != 'glob' and entry['host'] == relpath:
            g = _guest_for(entry)
            if g:
                return g

    # 2. glob パターン (フルパスで照合)
    for entry in files:
        if entry.get('type', 'file') == 'glob' and \
                fnmatch.fnmatch(relpath, entry['host']):
            g = _guest_for(entry)
            if g:
                return g

    # 3. ベース名一致 (旧挙動の互換)
    for entry in files:
        if os.path.basename(entry['host']) == basename:
            g = _guest_for(entry)
            if g:
                return g

    return None


def do_push(local_path, remote_name=None, resolve=False):
    """ホットデプロイ (再起動不要) — tools/hotdeploy.py に委譲

    旧実装は名前付きパイプ経由で rshell の `upload` コマンドへ hex を
    流し込んでいたが、`upload` はゲスト側で削除済みで常に失敗していた。
    現在は NP21/W 内蔵 aidebug の POST /api/mem でステージングバッファへ
    直接書き、rshell の `hotdeploy` でファイル化する。
    設計: docs/tasks/hotdeploy/DESIGN.md
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from hotdeploy import push

    guest = None
    if not resolve and remote_name:
        guest = remote_name if remote_name.startswith('/') else '/' + remote_name
    return push(local_path, guest)



def main():
    if len(sys.argv) < 2:
        print("NHD ext2 Deploy Tool (mount版)")
        print("")
        print("使い方: {} <command>".format(sys.argv[0]))
        print("")
        print("  sync [--tag TAG]       — deploy.yaml に基づくフルデプロイ")
        print("  push [--resolve] <file> [guest] — シリアル経由ホットデプロイ (再起動不要)")
        print("  mount                  — ext2パーティションをマウント")
        print("  umount                 — アンマウント")
        print("  copy [--dest DIR] [--rename NAME] <src> [...] — ファイルをext2にコピー")
        print("  copy-all [--dest DIR] <dir> [ext]   — dirの全ファイルを一括コピー")
        print("  setup-dirs             — システムディレクトリを作成")
        print("  ls [path]              — ファイル一覧")
        print("  rm <file>              — ファイル削除")
        print("  deploy                 — umount + NHDをNP21/Wにコピー")
        print("  format                 — ext2を再フォーマット (全消去)")
        print("  init                   — Windows側NHDをコピー+フォーマット+マウント")
        print("")
        print("パス:")
        print("  NHDローカル:  {}".format(NHD_LOCAL))
        print("  NHD NP21/W:   {}".format(NHD_REMOTE))
        print("  マウント:     {}".format(MOUNT_POINT))
        print("  deploy defs:  {}".format(", ".join(DEPLOY_MANIFESTS)))
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
        if not do_deploy():
            sys.exit(1)

    elif cmd == 'write-boot':
        if len(sys.argv) < 3:
            print("Usage: write-boot <loader.bin>")
            return
        loader = sys.argv[2]
        if not os.path.isfile(loader):
            print("Error: {} not found".format(loader))
            return
        do_write_boot(loader)

    elif cmd == 'format':
        do_format()

    elif cmd == 'pull':
        if not do_pull():
            sys.exit(1)

    elif cmd == 'init':
        do_init()

    elif cmd == 'sync':
        # --tag TAG オプションをパース
        tag_filter = None
        i = 2
        while i < len(sys.argv):
            if sys.argv[i] == '--tag' and i + 1 < len(sys.argv):
                tag_filter = sys.argv[i + 1]
                i += 2
            else:
                i += 1
        if do_sync(tag_filter=tag_filter) is False:
            sys.exit(1)

    elif cmd == 'sync-from-hostdrv':
        do_sync_from_hostdrv()

    elif cmd == 'push':
        # --resolve オプションをパース
        resolve = False
        args = []
        i = 2
        while i < len(sys.argv):
            if sys.argv[i] == '--resolve':
                resolve = True
                i += 1
            else:
                args.append(sys.argv[i])
                i += 1
        if not args:
            print("Usage: push [--resolve] <local_file> [guest_name]")
            return
        local_file = args[0]
        guest_name = args[1] if len(args) > 1 else None
        do_push(local_file, remote_name=guest_name, resolve=resolve)

    else:
        print("Unknown command: {}".format(cmd))


if __name__ == '__main__':
    main()
