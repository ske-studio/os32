#!/usr/bin/env python3
"""
OS32 HTTP & シリアルコンソールサーバー (os32_server.py)
Windows上のNP21Wの名前付きパイプ (\\.\pipe\np21w_com1) と常時接続し、
WSLなど外部からの HTTPリクエスト を受け取ってOS32と通信する。

機能:
  - POST /cmd  — OS32のrshellにコマンドを送信し結果を取得
  - GET /screenshot — NP21/Wのスクリーンショットを撮影

実行:
    C:\Python313\python.exe os32_server.py
"""

import sys
import os
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
import urllib.parse
import queue
import subprocess

try:
    import win32pipe
    import win32file
    import pywintypes
except ImportError:
    print("Error: pywin32 module is required.")
    sys.exit(1)

PIPE_NAME = r'\\.\pipe\np21w_com1'
HTTP_PORT = 8032

pipe_handle = None
pipe_write_lock = threading.Lock()
rx_queue = queue.Queue()

def connect_to_pipe():
    global pipe_handle
    if pipe_handle is not None:
        try: win32file.CloseHandle(pipe_handle)
        except: pass
        pipe_handle = None
    
    print(f"Connecting to {PIPE_NAME}...")
    while True:
        try:
            handle = win32file.CreateFile(
                PIPE_NAME,
                win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0, None,
                win32file.OPEN_EXISTING,
                0, None
            )
            win32pipe.SetNamedPipeHandleState(
                handle,
                win32pipe.PIPE_READMODE_BYTE | win32pipe.PIPE_WAIT,
                None, None
            )
            print("Connected to NP21/W pipe.")
            pipe_handle = handle
            return True
        except pywintypes.error as e:
            if e.winerror == 2:
                time.sleep(2)
            elif e.winerror == 231:
                try:
                    win32pipe.WaitNamedPipe(PIPE_NAME, 2000)
                except pywintypes.error:
                    time.sleep(1)
            else:
                print(f"Pipe connect error: {e}")
                time.sleep(2)

def _pipe_write_bytes(b_data):
    """パイプへバイナリを書き込む（ロック付き、ディレイ付き）"""
    global pipe_handle
    with pipe_write_lock:
        if not pipe_handle: return False
        try:
            for i in range(len(b_data)):
                win32file.WriteFile(pipe_handle, bytes([b_data[i]]))
                if (i % 64) == 63:
                    win32file.FlushFileBuffers(pipe_handle)
                    time.sleep(0.01) # 64文字ごとに10msウェイト
            win32file.FlushFileBuffers(pipe_handle)
            return True
        except pywintypes.error as e:
            print(f"Pipe write error: {e}")
            pipe_handle = None
            return False

def pipe_reader_thread():
    """パイプ受信監視スレッド — 全バイトをrx_queueに転送"""
    global pipe_handle
    connect_to_pipe()
    
    while True:
        if pipe_handle is None:
            connect_to_pipe()
            continue
            
        try:
            # PeekNamedPipeでデータ有無を確認してブロックを防ぐ
            try:
                hr, avail, message_avail = win32pipe.PeekNamedPipe(pipe_handle, 0)
            except pywintypes.error as e:
                # ERROR_BROKEN_PIPE
                if e.winerror == 109:
                    pipe_handle = None
                    continue
                raise
                
            if avail > 0:
                hr, data = win32file.ReadFile(pipe_handle, 1)
            else:
                time.sleep(0.01)
                continue
                
            if not data: continue
            b = data[0]
            rx_queue.put(b)
                
        except pywintypes.error as e:
            if e.winerror == 109: pipe_handle = None
            elif e.winerror == 232: time.sleep(0.01)
            else:
                print(f"Pipe read error: {e}")
                pipe_handle = None
                time.sleep(1)


def send_and_wait_eot(cmd_str, timeout=30):
    """HTTP要求から呼ばれる。rshell向けにコマンドを送信して結果の文字列を収集する。"""
    # clear rx queue
    while not rx_queue.empty():
        try: rx_queue.get_nowait()
        except: pass
        
    cmd_bytes = cmd_str.encode('ascii', errors='replace') + b'\n'
    if not _pipe_write_bytes(cmd_bytes):
        return "Error: Cannot write to pipe."
        
    response = bytearray()
    start_time = time.time()
    
    while time.time() - start_time < timeout:
        try:
            b = rx_queue.get(timeout=0.1)
            if b == 0x04: # EOT
                return response.decode('utf-8', errors='replace')
            response.append(b)
        except queue.Empty:
            continue
            
    return "Error: Timeout waiting for EOT.\n" + response.decode('utf-8', errors='replace')


class OS32RequestHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        post_data = self.rfile.read(content_length)
        parsed_path = urllib.parse.urlparse(self.path)
        
        if parsed_path.path == '/cmd':
            cmd = post_data.decode('utf-8', errors='replace').strip()
            output = send_and_wait_eot(cmd)
            self.send_response(200)
            self.send_header('Content-type', 'text/plain; charset=utf-8')
            self.end_headers()
            self.wfile.write(output.encode('utf-8', errors='replace'))
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Endpoint not found.")

    def do_GET(self):
        parsed_path = urllib.parse.urlparse(self.path)
        if parsed_path.path == '/screenshot':
            # Run screenshot.py to capture
            script_dir = os.path.dirname(os.path.abspath(__file__))
            screenshot_script = os.path.join(script_dir, "screenshot.py")
            output_png = os.path.join(script_dir, "temp_screenshot.png")
            
            try:
                subprocess.run([sys.executable, screenshot_script, output_png], check=False)
                if os.path.exists(output_png):
                    with open(output_png, "rb") as f:
                        img_data = f.read()
                    
                    self.send_response(200)
                    self.send_header('Content-type', 'image/png')
                    self.send_header('Content-Length', str(len(img_data)))
                    self.end_headers()
                    self.wfile.write(img_data)
                    
                    try:
                        os.remove(output_png)
                    except:
                        pass
                else:
                    self.send_response(500)
                    self.end_headers()
                    self.wfile.write(b"Failed to capture screenshot.")
            except Exception as e:
                self.send_response(500)
                self.end_headers()
                self.wfile.write(f"Screenshot error: {e}".encode('utf-8'))
        else:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Endpoint not found.")
            
    def log_message(self, format, *args):
        pass # Suppress default HTTP logging

def run_server():
    server_address = ('', HTTP_PORT)
    httpd = HTTPServer(server_address, OS32RequestHandler)
    print(f"OS32 HTTP & Serial Console Server running on port {HTTP_PORT}...")
    
    t = threading.Thread(target=pipe_reader_thread, daemon=True)
    t.start()
    
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        httpd.server_close()

if __name__ == '__main__':
    run_server()
