#!/usr/bin/env python3
"""
upload.py — OS32へのバイナリファイルアップロード (rshell経由)

ホストからシリアル(名前付きパイプ)経由でバイナリファイルをOS32に転送する。
OS32側の upload コマンドと連携し、hexエンコードで送信。

使い方:
  python3 upload.py <local_file> [remote_name]
"""

import sys
import os
import subprocess
import time
import tempfile

PIPE_NAME = 'np21w_com1'

def upload_file(local_path, remote_name):
    """バイナリファイルをOS32にアップロード"""
    
    with open(local_path, 'rb') as f:
        data = f.read()
    
    size = len(data)
    size_hex = format(size, 'x')
    hex_data = data.hex()
    
    print(f"Uploading {local_path} ({size} bytes) as {remote_name}...")
    
    cmd_str = f" upload {remote_name} {size_hex}"
    cmd_bytes = cmd_str.encode('ascii') + b'\n'
    hex_bytes_data = hex_data.encode('ascii')
    
    # 全送信データを一時ファイルに保存
    all_data = cmd_bytes + hex_bytes_data
    
    # Windows側一時ファイルを使用
    tmp_path = '/mnt/c/WATCOM/tmp_upload.bin'
    win_tmp_path = 'C:\\WATCOM\\tmp_upload.bin'
    
    with open(tmp_path, 'wb') as f:
        f.write(all_data)
    
    cmd_len = len(cmd_bytes)
    
    # PowerShellスクリプト: ファイルから読み込んで送信
    ps = (
        "$ErrorActionPreference = 'Stop'; "
        "try { "
        "  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', "
        f"'{PIPE_NAME}', [System.IO.Pipes.PipeDirection]::InOut); "
        "  $pipe.Connect(5000); "
        f"  $data = [System.IO.File]::ReadAllBytes('{win_tmp_path}'); "
        # コマンド部分を送信
        f"  $pipe.Write($data, 0, {cmd_len}); "
        "  $pipe.Flush(); "
        "  Start-Sleep -Milliseconds 500; "
        # hexデータを64バイトずつ送信
        f"  for ($i = {cmd_len}; $i -lt $data.Length; $i += 64) {{ "
        "    $end = [Math]::Min($i + 64, $data.Length); "
        "    $len = $end - $i; "
        "    $pipe.Write($data, $i, $len); "
        "    if (($i % 1024) -eq 0) { $pipe.Flush(); Start-Sleep -Milliseconds 2; } "
        "  }; "
        "  $pipe.Flush(); "
        # 応答読み取り
        "  $buf = New-Object byte[] 1; "
        "  $sb = New-Object System.Text.StringBuilder; "
        "  $timeout = [DateTime]::Now.AddSeconds(8); "
        "  while ([DateTime]::Now -lt $timeout) { "
        "    if ($pipe.Read($buf, 0, 1) -gt 0) { "
        "      if ($buf[0] -eq 4) { break; } "
        "      [void]$sb.Append([char]$buf[0]); "
        "      $timeout = [DateTime]::Now.AddSeconds(3); "
        "    } "
        "  }; "
        "  Write-Host $sb.ToString(); "
        "  $pipe.Close(); "
        "} catch { "
        "  Write-Host ('ERROR: ' + $_); "
        "}"
    )
    
    result = subprocess.run(
        ['powershell.exe', '-NoProfile', '-Command', ps],
        capture_output=True, text=True, timeout=60
    )
    
    # 一時ファイル削除
    try:
        os.unlink(tmp_path)
    except OSError:
        pass
    
    output = result.stdout.strip()
    if output:
        print(output)
    
    if 'OK' in output:
        print(f"Upload successful: {remote_name} ({size} bytes)")
        return True
    else:
        print(f"Upload may have failed")
        if result.stderr:
            print(f"stderr: {result.stderr.strip()}")
        return False

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <local_file> [remote_name]")
        sys.exit(1)
    
    local_path = sys.argv[1]
    remote_name = sys.argv[2] if len(sys.argv) > 2 else os.path.basename(local_path)
    
    if not os.path.isfile(local_path):
        print(f"Error: {local_path} not found")
        sys.exit(1)
    
    upload_file(local_path, remote_name)

if __name__ == '__main__':
    main()
