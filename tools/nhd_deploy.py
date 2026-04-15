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
  python3 nhd_deploy.py sync [--tag TAG]   — deploy.yaml に基づくフルデプロイ
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
import yaml

# === パス設定 ===
NHD_LOCAL = "/tmp/os32.nhd"
NHD_REMOTE = r"/mnt/c/Users/hight/OneDrive/ドキュメント/np21w/os32.nhd"
MOUNT_POINT = "/tmp/os32"

# プロジェクトルート (tools/ の親ディレクトリ)
PROJ_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPLOY_YAML = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            'deploy.yaml')

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


def load_deploy_yaml():
    """deploy.yaml を読み込んで返す"""
    if not os.path.isfile(DEPLOY_YAML):
        print("Error: {} が見つかりません".format(DEPLOY_YAML), file=sys.stderr)
        return None
    with open(DEPLOY_YAML, 'r', encoding='utf-8') as f:
        return yaml.safe_load(f)


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

    1. ブート領域書き込み (write-kernel)
    2. ディレクトリ構造作成
    3. 全ファイルコピー
    """
    cfg = load_deploy_yaml()
    if cfg is None:
        return False

    print("=" * 55)
    print("  OS32 フルデプロイ (deploy.yaml)")
    if tag_filter:
        print("  タグフィルタ: {}".format(tag_filter))
    print("=" * 55)

    # === Phase 1: ブート領域 ===
    if not tag_filter:
        boot = cfg.get('boot', {})
        kernel_path = os.path.join(PROJ_DIR, boot.get('kernel', 'kernel.bin'))
        loader_path = os.path.join(PROJ_DIR, boot.get('loader', ''))

        if os.path.isfile(kernel_path):
            loader_arg = loader_path if os.path.isfile(loader_path) else None
            print("\n[boot] カーネル+ローダー書き込み")
            do_write_kernel(kernel_path, loader_arg)
        else:
            print("Warning: カーネル {} が見つかりません".format(kernel_path))

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


def resolve_guest_path(host_file):
    """deploy.yaml からホストファイルに対応するゲストパスを解決する

    Returns: ゲストパス文字列 (見つからなければ None)
    """
    cfg = load_deploy_yaml()
    if cfg is None:
        return None

    basename = os.path.basename(host_file)
    fs = cfg.get('filesystem', {})
    files = fs.get('files', [])

    for entry in files:
        entry_type = entry.get('type', 'file')
        exclude = entry.get('exclude', [])
        guest = entry['guest']

        if entry_type == 'glob':
            # glob パターンにマッチするか確認
            import fnmatch
            host_pattern = entry['host']
            pattern_basename = os.path.basename(host_pattern)
            if fnmatch.fnmatch(basename, pattern_basename):
                if basename in exclude:
                    continue
                if guest.endswith('/'):
                    return guest + basename
                else:
                    return guest
        else:
            # 完全一致
            if os.path.basename(entry['host']) == basename:
                return guest

    return None


def do_push(local_path, remote_name=None, resolve=False):
    """シリアル経由ホットデプロイ (再起動不要)

    名前付きパイプ経由で rshell の upload コマンドを使い、
    実行中の OS32 にファイルを転送する。

    Args:
        local_path: ホスト側ファイルパス
        remote_name: ゲスト側ファイル名 (Noneならbasenameを使用)
        resolve: Trueなら deploy.yaml からゲストパスを自動解決
    """
    PIPE_NAME = 'np21w_com1'

    if not os.path.isfile(local_path):
        print("Error: {} が見つかりません".format(local_path), file=sys.stderr)
        return False

    # ゲストパス解決
    if resolve:
        guest_path = resolve_guest_path(local_path)
        if guest_path:
            remote_name = guest_path.lstrip('/')
            print("deploy.yaml から解決: {} -> /{}".format(
                os.path.basename(local_path), remote_name))
        else:
            print("Warning: deploy.yaml にマッチなし、ファイル名をそのまま使用")
            remote_name = os.path.basename(local_path)
    elif remote_name is None:
        remote_name = os.path.basename(local_path)

    with open(local_path, 'rb') as f:
        data = f.read()

    size = len(data)
    size_hex = format(size, 'x')
    hex_data = data.hex()

    print("Push: {} ({} bytes) -> /{}".format(
        os.path.basename(local_path), size, remote_name))

    cmd_str = " upload {} {}".format(remote_name, size_hex)
    cmd_bytes = cmd_str.encode('ascii') + b'\n'
    hex_bytes_data = hex_data.encode('ascii')

    # 全送信データを一時ファイルに保存
    all_data = cmd_bytes + hex_bytes_data
    tmp_path = '/mnt/c/WATCOM/tmp_upload.bin'
    win_tmp_path = 'C:\\WATCOM\\tmp_upload.bin'

    with open(tmp_path, 'wb') as f:
        f.write(all_data)

    cmd_len = len(cmd_bytes)

    # PowerShellスクリプト: 名前付きパイプ経由で送信
    ps = (
        "$ErrorActionPreference = 'Stop'; "
        "try {{ "
        "  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', "
        "'{pipe}', [System.IO.Pipes.PipeDirection]::InOut); "
        "  $pipe.Connect(5000); "
        "  $data = [System.IO.File]::ReadAllBytes('{tmp}'); "
        "  $pipe.Write($data, 0, {cmd_len}); "
        "  $pipe.Flush(); "
        "  Start-Sleep -Milliseconds 500; "
        "  for ($i = {cmd_len}; $i -lt $data.Length; $i += 64) {{ "
        "    $end = [Math]::Min($i + 64, $data.Length); "
        "    $len = $end - $i; "
        "    $pipe.Write($data, $i, $len); "
        "    if (($i % 1024) -eq 0) {{ $pipe.Flush(); Start-Sleep -Milliseconds 2; }} "
        "  }}; "
        "  $pipe.Flush(); "
        "  $buf = New-Object byte[] 1; "
        "  $sb = New-Object System.Text.StringBuilder; "
        "  $timeout = [DateTime]::Now.AddSeconds(8); "
        "  while ([DateTime]::Now -lt $timeout) {{ "
        "    if ($pipe.Read($buf, 0, 1) -gt 0) {{ "
        "      if ($buf[0] -eq 4) {{ break; }} "
        "      [void]$sb.Append([char]$buf[0]); "
        "      $timeout = [DateTime]::Now.AddSeconds(3); "
        "    }} "
        "  }}; "
        "  Write-Host $sb.ToString(); "
        "  $pipe.Close(); "
        "}} catch {{ "
        "  Write-Host ('ERROR: ' + $_); "
        "}}"
    ).format(pipe=PIPE_NAME, tmp=win_tmp_path, cmd_len=cmd_len)

    print("転送中...")
    try:
        result = subprocess.run(
            ['powershell.exe', '-NoProfile', '-Command', ps],
            capture_output=True, text=True, timeout=60
        )
    except subprocess.TimeoutExpired:
        print("Error: タイムアウト (60秒)", file=sys.stderr)
        return False
    except FileNotFoundError:
        print("Error: powershell.exe が見つかりません", file=sys.stderr)
        return False

    # 一時ファイル削除
    try:
        os.unlink(tmp_path)
    except OSError:
        pass

    output = result.stdout.strip()
    if output:
        print(output)

    if 'OK' in output:
        print("Push 成功: /{} ({} bytes)".format(remote_name, size))
        return True
    else:
        print("Push 失敗の可能性があります")
        if result.stderr:
            print("stderr: {}".format(result.stderr.strip()))
        return False


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
        print("  write-kernel <k> [ldr] — カーネルをブート領域に書き込み")
        print("  format                 — ext2を再フォーマット (全消去)")
        print("  init                   — Windows側NHDをコピー+フォーマット+マウント")
        print("")
        print("パス:")
        print("  NHDローカル:  {}".format(NHD_LOCAL))
        print("  NHD NP21/W:   {}".format(NHD_REMOTE))
        print("  マウント:     {}".format(MOUNT_POINT))
        print("  deploy.yaml:  {}".format(DEPLOY_YAML))
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
        do_sync(tag_filter=tag_filter)

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
