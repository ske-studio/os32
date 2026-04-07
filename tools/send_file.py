#!/usr/bin/env python3
"""
OS32シリアルファイル転送ツール (WSL → NP21/W Named Pipe → OS32)

使い方:
    python3 send_file.py <ファイルパス>

手順:
    1. OS32で 'serial' を実行してRS-232C初期化
    2. OS32で 'recv' を実行して受信待ち開始
    3. WSLで python3 send_file.py <ファイル> を実行
"""

import sys
import os
import struct
import subprocess

def send_file(filepath):
    if not os.path.exists(filepath):
        print(f"エラー: ファイルが見つかりません: {filepath}")
        return False

    with open(filepath, 'rb') as f:
        data = f.read()

    filesize = len(data)
    filename = os.path.basename(filepath)

    # 8.3形式に収める
    if len(filename) > 12:
        name, ext = os.path.splitext(filename)
        filename = name[:8] + ext[:4]

    print(f"ファイル: {filename} ({filesize} bytes)")

    # パケット構築
    packet = bytearray()
    packet.extend(b'OS32')                                    # マジック

    fname_bytes = filename.encode('ascii', errors='replace')[:12]
    packet.extend(fname_bytes)
    packet.extend(b'\x00' * (12 - len(fname_bytes)))          # ファイル名 (12B)

    packet.extend(struct.pack('<I', filesize))                 # サイズ (4B LE)
    packet.extend(data)                                       # データ

    checksum = sum(data) & 0xFF
    packet.append(checksum)                                   # チェックサム (1B)
    packet.extend(b'END')                                     # フッタ

    print(f"チェックサム: 0x{checksum:02X}")
    print(f"パケット: {len(packet)} bytes")

    # Windows側の一時ファイルに保存
    win_tmp = r'C:\WATCOM\src\os32\tools\os32_packet.bin'
    wsl_tmp = '/mnt/c/WATCOM/src/os32/tools/os32_packet.bin'
    with open(wsl_tmp, 'wb') as f:
        f.write(packet)

    # PowerShellコマンドを直接実行
    ps_cmd = (
        "$ErrorActionPreference = 'Stop'; "
        "try { "
        "  $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'np21w_com1', 'Out'); "
        "  $pipe.Connect(5000); "
        f"  $data = [System.IO.File]::ReadAllBytes('{win_tmp}'); "
        "  Write-Host ('パイプ接続OK. 送信中... ' + $data.Length + ' bytes'); "
        "  for ($i = 0; $i -lt $data.Length; $i += 16) { "
        "    $end = [Math]::Min($i + 16, $data.Length); "
        "    $chunk = $data[$i..($end-1)]; "
        "    $pipe.Write($chunk, 0, $chunk.Length); "
        "    Start-Sleep -Milliseconds 5; "
        "  }; "
        "  $pipe.Flush(); "
        "  $pipe.Close(); "
        "  Write-Host '送信完了!' "
        "} catch { "
        "  Write-Host ('エラー: ' + $_) "
        "}"
    )

    print("送信中...")
    result = subprocess.run(
        ['powershell.exe', '-NoProfile', '-Command', ps_cmd],
        capture_output=True, timeout=30
    )
    print(result.stdout.decode('utf-8', errors='replace').strip())
    if result.stderr.strip():
        print(f"stderr: {result.stderr.decode('utf-8', errors='replace').strip()}")

    return True

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("使い方: python3 send_file.py <ファイルパス>")
        sys.exit(1)

    send_file(sys.argv[1])
