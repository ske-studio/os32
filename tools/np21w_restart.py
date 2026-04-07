#!/usr/bin/env python3
"""
np21w_restart.py — NP21/Wを停止→INI書き換え→再起動

使い方:
    python3 np21w_restart.py [fdd1_path]
    
fdd1_pathが指定されればFDD1FILEを更新。
省略時はINIのFDD1FILEはそのまま。
"""

import sys
import os
import subprocess
import time
import re

NP21W_DIR = r'C:\Users\hight\OneDrive\ドキュメント\np21w'
NP21W_EXE = os.path.join(NP21W_DIR, 'np21x64w.exe')
NP21W_INI = os.path.join(NP21W_DIR, 'np21x64w.ini')

# WSLパス変換
def to_win_path(wsl_path):
    """WSLパスをWindowsパスに変換"""
    if wsl_path.startswith('/mnt/'):
        drive = wsl_path[5].upper()
        rest = wsl_path[6:].replace('/', '\\')
        return f'{drive}:{rest}'
    return wsl_path

def stop_np21w():
    """NP21/Wプロセスを停止"""
    print("  NP21/W停止...", end='', flush=True)
    subprocess.run(
        ['powershell.exe', '-NoProfile', '-Command',
         "Stop-Process -Name 'np21x64w' -Force -ErrorAction SilentlyContinue"],
        capture_output=True, timeout=10
    )
    time.sleep(2)
    print(" OK")

def update_ini_fdd1(fdd1_path):
    """INIファイルのFDD1FILEを更新"""
    win_path = to_win_path(fdd1_path) if fdd1_path.startswith('/') else fdd1_path
    
    # INIをWindowsパスで読んで書き換え
    ini_wsl = NP21W_INI.replace('\\', '/').replace('C:', '/mnt/c')
    
    try:
        with open(ini_wsl, 'rb') as f:
            content = f.read()
        
        # FDD1FILEの行を書き換え (Shift-JIS混在のバイナリ処理)
        old_pattern = b'FDD1FILE='
        idx = content.find(old_pattern)
        if idx >= 0:
            # 行末を探す
            end_idx = content.find(b'\r\n', idx)
            if end_idx < 0:
                end_idx = content.find(b'\n', idx)
            if end_idx < 0:
                end_idx = len(content)
            
            new_line = b'FDD1FILE=' + win_path.encode('ascii', errors='replace')
            content = content[:idx] + new_line + content[end_idx:]
            
            with open(ini_wsl, 'wb') as f:
                f.write(content)
            
            print(f"  FDD1FILE={win_path}")
        else:
            print("  WARNING: FDD1FILE not found in INI")
    except Exception as e:
        print(f"  INI更新エラー: {e}")

def start_np21w():
    """NP21/Wを起動"""
    print("  NP21/W起動...", end='', flush=True)
    subprocess.run(
        ['powershell.exe', '-NoProfile', '-Command',
         f"Start-Process -FilePath '{NP21W_EXE}' -WorkingDirectory '{NP21W_DIR}'"],
        capture_output=True, timeout=10
    )
    print(" OK")

def main():
    fdd1 = None
    if len(sys.argv) > 1:
        fdd1 = sys.argv[1]
    
    print("=== NP21/W 再起動 ===")
    stop_np21w()
    
    if fdd1:
        update_ini_fdd1(fdd1)
    
    start_np21w()
    print("=== 完了 ===")

if __name__ == '__main__':
    main()
