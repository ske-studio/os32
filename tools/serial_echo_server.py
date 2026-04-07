"""
serial_echo_server.py — NP21/W COM1 接続用名前付きパイプエコーサーバー

NP21/Wの [Device] > [Serial/Parallel option...] で:
  COM1: \\.\pipe\np21w_com1
に設定して使用する。

使い方:
  python serial_echo_server.py

受信した文字をそのままエコーバックし、画面にも表示する。
"""

import win32pipe
import win32file
import pywintypes
import time
import sys

PIPE_NAME = r'\\.\pipe\np21w_com1'

def create_server():
    """名前付きパイプサーバーを作成"""
    print(f"[Server] Creating named pipe: {PIPE_NAME}")
    pipe = win32pipe.CreateNamedPipe(
        PIPE_NAME,
        win32pipe.PIPE_ACCESS_DUPLEX,
        win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_READMODE_BYTE | win32pipe.PIPE_WAIT,
        1,      # 最大インスタンス数
        4096,   # 出力バッファ
        4096,   # 入力バッファ
        0,      # デフォルトタイムアウト
        None    # セキュリティ属性
    )
    return pipe

def main():
    print("=" * 50)
    print(" NP21/W Serial Echo Server")
    print(" Named Pipe: " + PIPE_NAME)
    print("=" * 50)
    print()
    print("[Server] NP21/Wで以下の設定を行ってください:")
    print("  [Device] > [Serial/Parallel option...]")
    print(f"  COM1 = {PIPE_NAME}")
    print()

    pipe = create_server()
    print("[Server] Waiting for NP21/W to connect...")

    try:
        # クライアント(NP21/W)の接続を待つ
        win32pipe.ConnectNamedPipe(pipe, None)
        print("[Server] Connected!")
        print("[Server] Echoing received data (Ctrl+C to stop)")
        print("-" * 50)

        while True:
            try:
                # データ受信
                hr, data = win32file.ReadFile(pipe, 1024)
                if data:
                    text = data.decode('ascii', errors='replace')
                    sys.stdout.write(f"[RX] {repr(text)}\n")
                    sys.stdout.flush()

                    # エコーバック
                    win32file.WriteFile(pipe, data)

                    # 特別なコマンド処理
                    stripped = text.strip()
                    if stripped == "HELLO":
                        response = b"Hello from Host PC!\r\n"
                        win32file.WriteFile(pipe, response)
                        print(f"[TX] {repr(response.decode())}")
                    elif stripped == "TIME":
                        response = f"Host time: {time.strftime('%H:%M:%S')}\r\n".encode()
                        win32file.WriteFile(pipe, response)
                        print(f"[TX] {repr(response.decode())}")

            except pywintypes.error as e:
                if e.args[0] == 109:  # ERROR_BROKEN_PIPE
                    print("[Server] Client disconnected")
                    break
                raise

    except KeyboardInterrupt:
        print("\n[Server] Shutting down...")
    finally:
        win32file.CloseHandle(pipe)
        print("[Server] Pipe closed")

if __name__ == "__main__":
    main()
