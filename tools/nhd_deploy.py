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
NHD_HEADER = 512

def run_debugfs(nhd_path, commands, writable=False):
    """debugfs をRAWイメージに対して実行する"""
    # debugfsはオフセットをサポートしないため、一時RAWファイルを作成
    # ただしシンボリックリンクでは不可なので、直接操作
    
    # 高速化: NHDからRAWを作成 (bs=512 skip=1)
    tmp = tempfile.NamedTemporaryFile(suffix='.raw', delete=False)
    tmp.close()
    
    try:
        # 抽出 (bs=512で高速コピー)
        subprocess.run(
            ['dd', f'if={nhd_path}', f'of={tmp.name}',
             'bs=512', 'skip=1', 'status=none'],
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
                input=cmd_str, capture_output=True, text=True
            )
        else:
            # 単一コマンド
            result = subprocess.run(
                args + ['-R', commands, tmp.name],
                capture_output=True, text=True
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
            # ヘッダ保存
            with open(nhd_path, 'rb') as f:
                header = f.read(NHD_HEADER)
            
            # ヘッダ + 変更済みRAW
            with open(nhd_path + '.tmp', 'wb') as f:
                f.write(header)
                with open(tmp.name, 'rb') as raw:
                    shutil.copyfileobj(raw, f)
            
            os.replace(nhd_path + '.tmp', nhd_path)
    
    finally:
        os.unlink(tmp.name)
    
    return result.returncode

def main():
    if len(sys.argv) < 2:
        print("NHD ext2 Deploy Tool (sudo不要)")
        print(f"Usage: {sys.argv[0]} {{copy|ls|cat|rm}}")
        print("  copy <src> [dest]  — ファイルコピー")
        print("  ls [path]          — ファイル一覧")
        print("  cat <file>         — ファイル表示")
        print("  rm <file>          — ファイル削除")
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
        # debugfsの write コマンドはフルパスが必要
        src_abs = os.path.abspath(src)
        run_debugfs(NHD_FILE, f'write {src_abs} {dest}', writable=True)
        print("Done!")
    
    elif cmd == 'rm':
        if len(sys.argv) < 3:
            print("Usage: rm <file>")
            return
        print(f"Removing {sys.argv[2]}...")
        run_debugfs(NHD_FILE, f'rm {sys.argv[2]}', writable=True)
        print("Done!")
    
    else:
        print(f"Unknown command: {cmd}")

if __name__ == '__main__':
    main()
