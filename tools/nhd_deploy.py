#!/usr/bin/env python3
"""
nhd_deploy.py — NHDイメージのext2にファイルをコピー (sudo不要)

debugfsを使ってext2を直接操作する。
NHDヘッダ(512B)をスキップしたオフセットでdebugfsを実行。

使い方:
  python3 nhd_deploy.py copy <src_file> [dest_name]
  python3 nhd_deploy.py ls [path]
"""

import sys
import os
import subprocess
import shutil
import tempfile
import time

NHD_FILE = r"/mnt/c/Users/hight/OneDrive/ドキュメント/np21w/os32.nhd"

MAX_RETRY = 3
RETRY_WAIT = 3  # 秒

def ensure_nhd_unlocked(nhd_path):
    """NHDファイルがロックされていたらNP21/Wをkillしてアンロックする"""
    try:
        with open(nhd_path, 'r+b') as f:
            f.read(1)
        return True
    except PermissionError:
        print("NHDファイルがロックされています。NP21/Wを終了します...",
              file=sys.stderr)
        try:
            subprocess.run(['taskkill.exe', '/F', '/IM', 'np21x64w.exe'],
                           capture_output=True, timeout=10)
        except Exception:
            pass
        try:
            subprocess.run(['taskkill.exe', '/F', '/IM', 'np21w.exe'],
                           capture_output=True, timeout=10)
        except Exception:
            pass
        time.sleep(RETRY_WAIT)
        # 再確認
        try:
            with open(nhd_path, 'r+b') as f:
                f.read(1)
            print("ロック解除確認OK")
            return True
        except PermissionError:
            print("まだロックされています", file=sys.stderr)
            return False

# NHDヘッダ(512B) + ブート領域(LBA 0-271) = 273セクタ
# ext2パーティションは install.c の HDD_PARTITION_LBA=272 から開始
NHD_HEADER_SECTORS = 1      # NHDファイルヘッダ ("T98HDDIMAGE...")
HDD_PARTITION_LBA = 272     # ext2パーティション開始LBA (install.c と一致)
PARTITION_SKIP = NHD_HEADER_SECTORS + HDD_PARTITION_LBA  # = 273セクタ
PARTITION_OFFSET = PARTITION_SKIP * 512  # = 139776 バイト

def run_debugfs(nhd_path, commands, writable=False):
    """debugfs をRAWイメージに対して実行する"""
    # debugfsはオフセットをサポートしないため、一時RAWファイルを作成
    # ただしシンボリックリンクでは不可なので、直接操作
    
    # ext2パーティション部分のみ抽出 (bs=512 skip=PARTITION_SKIP)
    tmp = tempfile.NamedTemporaryFile(suffix='.raw', delete=False)
    tmp.close()
    
    try:
        # 抽出 (NHDヘッダ+ブート領域をスキップ)
        subprocess.run(
            ['dd', f'if={nhd_path}', f'of={tmp.name}',
             'bs=512', f'skip={PARTITION_SKIP}', 'status=none'],
            check=True
        )
        
        # debugfs実行
        args = ['debugfs']
        if writable:
            args.append('-w')
        
        if isinstance(commands, list):
            # 複数コマンド: stdinに書き込み
            cmd_str = '\n'.join(commands) + '\n'
            result = subprocess.run(
                args + [tmp.name],
                input=cmd_str, capture_output=True, text=True,
                timeout=60
            )
        else:
            # 単一コマンド
            result = subprocess.run(
                args + ['-R', commands, tmp.name],
                capture_output=True, text=True,
                timeout=60
            )
        
        if result.stdout:
            # "debugfs X.X.X" の行を除去
            lines = result.stdout.strip().split('\n')
            for line in lines:
                if not line.startswith('debugfs '):
                    print(line)
        
        if result.stderr:
            for line in result.stderr.strip().split('\n'):
                if 'debugfs' not in line.lower() and 'superblock' not in line.lower():
                    print(line, file=sys.stderr)
        
        # 書き込みモードなら書き戻し
        if writable:
            # NHDヘッダ+ブート領域を保存 (パーティション前の全データ)
            with open(nhd_path, 'rb') as f:
                pre_partition = f.read(PARTITION_OFFSET)
            
            # ヘッダ+ブート領域 + 変更済みext2パーティション
            with open(nhd_path + '.tmp', 'wb') as f:
                f.write(pre_partition)
                with open(tmp.name, 'rb') as raw:
                    shutil.copyfileobj(raw, f)
            
            os.replace(nhd_path + '.tmp', nhd_path)
    
    finally:
        os.unlink(tmp.name)
    
    return result.returncode

def write_kernel(nhd_path, kernel_bin, loader_bin=None):
    """NHDのブート領域にloader+kernelを直接書き込む
    
    NHDレイアウト (512B/セクタ):
      NHDヘッダ: 512B (オフセット0)
      LBA 0: IPL (boot_hdd.bin)
      LBA 1: パーティションテーブル
      LBA 2-5: loader_hdd.bin (最大4セクタ = 2048B)
      LBA 6+: kernel.bin
      LBA 272+: ext2パーティション
    """
    NHD_HEADER = 512  # NHDファイルヘッダ
    SECTOR = 512
    KERNEL_LBA = 6
    LOADER_LBA = 2
    
    kernel_offset = NHD_HEADER + KERNEL_LBA * SECTOR  # 3584
    loader_offset = NHD_HEADER + LOADER_LBA * SECTOR  # 1536
    
    with open(kernel_bin, 'rb') as f:
        kernel_data = f.read()
    
    print(f"  kernel.bin: {len(kernel_data)} bytes ({(len(kernel_data)+511)//512} sectors)")
    
    with open(nhd_path, 'r+b') as nhd:
        # loader書き込み (指定がある場合)
        if loader_bin and os.path.isfile(loader_bin):
            with open(loader_bin, 'rb') as f:
                loader_data = f.read()
            nhd.seek(loader_offset)
            nhd.write(loader_data)
            print(f"  loader:     {len(loader_data)} bytes -> LBA {LOADER_LBA}")
        
        # kernel書き込み
        nhd.seek(kernel_offset)
        nhd.write(kernel_data)
        print(f"  kernel:     {len(kernel_data)} bytes -> LBA {KERNEL_LBA}")
    
    print("Done!")

def main():
    if len(sys.argv) < 2:
        print("NHD ext2 Deploy Tool (sudo不要)")
        print(f"Usage: {sys.argv[0]} {{copy|copy-all|ls|cat|rm|write-kernel}}")
        print("  copy <src> [dest]        — ext2にファイルコピー")
        print("  copy <s1> <s2> ...       — 複数ファイルを一括コピー")
        print("  copy-all <dir> [ext]     — dirの全ファイルを一括コピー (既定: *.bin)")
        print("  ls [path]                — ext2ファイル一覧")
        print("  cat <file>               — ext2ファイル表示")
        print("  rm <file>                — ext2ファイル削除")
        print("  write-kernel <kern> [ldr] — カーネル/ローダーをブート領域に書き込み")
        return
    
    cmd = sys.argv[1]
    
    # 書き込み系コマンドの場合、ロック確認
    write_cmds = {'copy', 'copy-all', 'rm', 'mkdir', 'write-kernel'}
    if cmd in write_cmds:
        if not ensure_nhd_unlocked(NHD_FILE):
            print("Error: NHDファイルのロックを解除できません", file=sys.stderr)
            sys.exit(1)
    
    if cmd == 'ls':
        path = sys.argv[2] if len(sys.argv) > 2 else '/'
        print(f"=== ext2 ls {path} ===")
        run_debugfs(NHD_FILE, f'ls -l {path}')
    
    elif cmd == 'cat':
        if len(sys.argv) < 3:
            print("Usage: cat <file>")
            return
        run_debugfs(NHD_FILE, f'cat {sys.argv[2]}')
    
    elif cmd == 'copy':
        if len(sys.argv) < 3:
            print("Usage: copy <src_file> [dest_path]")
            print("       copy <src1> <src2> ...  (複数ファイル一括コピー)")
            return

        # 引数が2個 (copy src [dest]) の場合は従来動作
        # 引数が3個以上で最後がファイルの場合は複数ファイルモード
        src_files = sys.argv[2:]

        # 単一ファイル + 明示的dest指定のケースを判定
        # copy src dest (src がファイルで dest がファイルでない場合)
        if len(src_files) == 2 and os.path.isfile(src_files[0]) and not os.path.isfile(src_files[1]):
            # 従来の copy src dest モード
            src = src_files[0]
            dest = src_files[1]
            if not os.path.isfile(src):
                print(f"Error: {src} not found")
                return
            print(f"Copying {src} -> /{dest} ...")
            src_abs = os.path.abspath(src)
            run_debugfs(NHD_FILE, [f'rm {dest}', f'write {src_abs} {dest}'], writable=True)
            print("Done!")
        else:
            # 複数ファイル一括コピーモード: 1回のdebugfsセッションで処理
            valid_files = []
            for f in src_files:
                if os.path.isfile(f):
                    valid_files.append(f)
                else:
                    print(f"Warning: {f} not found, skipping")

            if not valid_files:
                print("Error: コピーするファイルがありません")
                return

            if len(valid_files) == 1:
                # 単一ファイル
                src = valid_files[0]
                dest = os.path.basename(src)
                print(f"Copying {src} -> /{dest} ...")
                src_abs = os.path.abspath(src)
                run_debugfs(NHD_FILE, [f'rm {dest}', f'write {src_abs} {dest}'], writable=True)
                print("Done!")
            else:
                # 複数ファイルバッチ処理
                print(f"=== Batch copy: {len(valid_files)} files ===")
                cmds = []
                for f in valid_files:
                    dest = os.path.basename(f)
                    src_abs = os.path.abspath(f)
                    cmds.append(f'rm {dest}')
                    cmds.append(f'write {src_abs} {dest}')
                    print(f"  {dest}")
                run_debugfs(NHD_FILE, cmds, writable=True)
                print(f"Done! ({len(valid_files)} files deployed)")

    elif cmd == 'copy-all':
        import glob as globmod
        if len(sys.argv) < 3:
            print("Usage: copy-all <dir> [extension]")
            print("  例: copy-all programs/       (*.bin を一括コピー)")
            print("  例: copy-all programs/ .bin")
            return
        src_dir = sys.argv[2]
        ext = sys.argv[3] if len(sys.argv) > 3 else '.bin'
        
        pattern = os.path.join(src_dir, f'*{ext}')
        files = sorted(globmod.glob(pattern))
        if not files:
            print(f"No {ext} files found in {src_dir}")
            return
        
        print(f"=== Batch copy: {len(files)} files from {src_dir} ===")
        
        # 全ファイルを1回のdebugfsセッションで処理
        cmds = []
        for f in files:
            dest = os.path.basename(f)
            src_abs = os.path.abspath(f)
            cmds.append(f'rm {dest}')
            cmds.append(f'write {src_abs} {dest}')
            print(f"  {dest}")
        
        run_debugfs(NHD_FILE, cmds, writable=True)
        print(f"Done! ({len(files)} files deployed)")
    
    elif cmd == 'rm':
        if len(sys.argv) < 3:
            print("Usage: rm <file>")
            return
        print(f"Removing {sys.argv[2]}...")
        run_debugfs(NHD_FILE, f'rm {sys.argv[2]}', writable=True)
        print("Done!")

    elif cmd == 'mkdir':
        if len(sys.argv) < 3:
            print("Usage: mkdir <dir>")
            return
        print(f"Creating directory {sys.argv[2]}...")
        run_debugfs(NHD_FILE, f'mkdir {sys.argv[2]}', writable=True)
        print("Done!")
    
    elif cmd == 'write-kernel':
        if len(sys.argv) < 3:
            print("Usage: write-kernel <kernel.bin> [loader_hdd.bin]")
            return
        kernel = sys.argv[2]
        loader = sys.argv[3] if len(sys.argv) > 3 else None
        if not os.path.isfile(kernel):
            print(f"Error: {kernel} not found")
            return
        print(f"Writing kernel to {NHD_FILE} ...")
        write_kernel(NHD_FILE, kernel, loader)
    
    else:
        print(f"Unknown command: {cmd}")

if __name__ == '__main__':
    main()

