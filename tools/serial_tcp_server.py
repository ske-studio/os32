"""
serial_tcp_server.py — シリアル通信テスト用TCPエコーサーバー

NP21/Wの [Device] > [Serial/Parallel option...] で
COM1を "TCP:localhost:8023" のようなTCP設定にするか、
com0comなどの仮想COMポートブリッジを使用する。

このサーバーは単純にTCPポート8023でリッスンし、
受信データをエコーバック + 特殊コマンドに応答する。

使い方 (WSL or Windows):
  python3 serial_tcp_server.py
"""

import socket
import time
import sys
import select

HOST = '0.0.0.0'
PORT = 8023

def main():
    print("=" * 50)
    print(" OS32 Serial Echo Server (TCP)")
    print(f" Listening on {HOST}:{PORT}")
    print("=" * 50)
    print()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((HOST, PORT))
    server.listen(1)

    print(f"[Server] Waiting for connection on port {PORT}...")

    try:
        conn, addr = server.accept()
        print(f"[Server] Connected from {addr}")
        print("[Server] Echoing received data (Ctrl+C to stop)")
        print("-" * 50)

        # ウェルカムメッセージ送信
        welcome = b"=== OS32 Host Server ===\r\n"
        conn.sendall(welcome)

        conn.setblocking(False)

        while True:
            try:
                # select で入力を待つ (タイムアウト0.1秒)
                readable, _, _ = select.select([conn, sys.stdin], [], [], 0.1)

                for r in readable:
                    if r is conn:
                        # NP21/Wからのデータ
                        data = conn.recv(1024)
                        if not data:
                            print("[Server] Client disconnected")
                            return

                        text = data.decode('ascii', errors='replace')
                        sys.stdout.write(f"[RX] {repr(text)}\n")
                        sys.stdout.flush()

                        # エコーバック
                        conn.sendall(data)

                        # コマンド処理
                        stripped = text.strip()
                        if stripped.upper() == "HELLO":
                            resp = b"Hello from Host PC!\r\n"
                            conn.sendall(resp)
                            print(f"[TX] {repr(resp.decode())}")
                        elif stripped.upper() == "TIME":
                            resp = f"Host time: {time.strftime('%H:%M:%S')}\r\n".encode()
                            conn.sendall(resp)
                            print(f"[TX] {repr(resp.decode())}")
                        elif stripped.upper() == "PING":
                            resp = b"PONG\r\n"
                            conn.sendall(resp)
                            print(f"[TX] PONG")

                    elif r is sys.stdin:
                        # キーボードからの入力をNP21/Wに送信
                        line = sys.stdin.readline()
                        if line:
                            conn.sendall(line.encode())
                            print(f"[TX] {repr(line)}")

            except BlockingIOError:
                pass
            except ConnectionResetError:
                print("[Server] Connection reset")
                return

    except KeyboardInterrupt:
        print("\n[Server] Shutting down...")
    finally:
        server.close()
        print("[Server] Server closed")

if __name__ == "__main__":
    main()
