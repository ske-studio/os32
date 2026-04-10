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

NHD_FILE = r"/mnt/c/Users/hight/OneDrive/ドキュメント/np21w/os32.nhd"

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
        print(f"Usage: {sys.argv[0]} {{copy|ls|cat|rm|write-kernel}}")
        print("  copy <src> [dest]        — ext2にファイルコピー")
        print("  ls [path]                — ext2ファイル一覧")
        print("  cat <file>               — ext2ファイル表示")
        print("  rm <file>                — ext2ファイル削除")
        print("  write-kernel <kern> [ldr] — カーネル/ローダーをブート領域に書き込み")
        return
    
    cmd = sys.argv[1]
    
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
            print("Usage: copy <src_file> [dest_name]")
            return
        src = sys.argv[2]
        dest = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(src)
        
        if not os.path.isfile(src):
            print(f"Error: {src} not found")
            return
        
        print(f"Copying {src} -> /{dest} ...")
        # debugfsの write は上書き不可のため、先に削除してから書き込む
        src_abs = os.path.abspath(src)
        run_debugfs(NHD_FILE, [f'rm {dest}', f'write {src_abs} {dest}'], writable=True)
        print("Done!")
    
    elif cmd == 'rm':
        if len(sys.argv) < 3:
            print("Usage: rm <file>")
            return
        print(f"Removing {sys.argv[2]}...")
        run_debugfs(NHD_FILE, f'rm {sys.argv[2]}', writable=True)
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
