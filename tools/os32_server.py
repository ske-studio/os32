#!/usr/bin/env python3
"""
OS32 統合 HTTP & SerialFS サーバー (os32_server.py)
Windows上のNP21Wの名前付きパイプ (\\.\pipe\np21w_com1) と常時接続し、
WSLなど外部からの HTTPリクエスト を受け取ってOS32と通信しつつ、
OS32からの自発的な SerialFS(VFS) ファイルアクセス要求にバックグラウンドで応答します。

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

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

def lzss_encode(data: bytes) -> bytes:
    if not data: return b''
    N = 4096
    F = 18
    THRESHOLD = 2
    
    text_buf = bytearray(b' ' * N)
    r = N - F
    encoded = bytearray()
    
    src_idx = 0
    src_len = len(data)
    
    flag_pos = 0
    flags = 0
    flag_bit = 1
    
    while src_idx < src_len:
        if flag_bit == 1:
            flag_pos = len(encoded)
            encoded.append(0)
            
        max_lookahead = min(F, src_len - src_idx)
        pattern = data[src_idx : src_idx + max_lookahead]
        
        match_len = 0
        match_pos = 0
        
        for i in range(N):
            # i が現在書き込み予定の r から r+F-1 の範囲にある場合は探索を除外する
            if (i - r) % N < F:
                continue
                
            match_count = 0
            while match_count < max_lookahead and text_buf[(i + match_count) % N] == data[src_idx + match_count]:
                match_count += 1
            if match_count > match_len:
                match_len = match_count
                match_pos = i
                if match_len == max_lookahead:
                    break
                    
        if match_len <= THRESHOLD:
            match_len = 1
            c = data[src_idx]
            encoded.append(c)
            flags |= flag_bit
            text_buf[r] = c
            r = (r + 1) % N
            src_idx += 1
        else:
            i_val = match_pos & 0xFF
            # Cのデコーダは展開時 for(k=0; k<=((j&0x0F)+THRESHOLD); k++) で (j&0xF)+3回のコピーを行うため
            # 展開長 - 3、つまり match_len - THRESHOLD - 1 を保存する。
            j_val = ((match_pos >> 4) & 0xF0) | ((match_len - THRESHOLD - 1) & 0x0F)
            encoded.append(i_val)
            encoded.append(j_val)
            
            for k in range(match_len):
                text_buf[r] = data[src_idx + k]
                r = (r + 1) % N
            src_idx += match_len
            
        flag_bit <<= 1
        if flag_bit > 128:
            encoded[flag_pos] = flags
            flags = 0
            flag_bit = 1
            
    if flag_bit > 1:
        encoded[flag_pos] = flags

    return bytes(encoded)

PIPE_NAME = r'\\.\pipe\np21w_com1'
HTTP_PORT = 8032
HOST_ROOT_DIR = r"C:\WATCOM" # SerialFSで/hostとして公開するホスト側のルート

pipe_handle = None
pipe_write_lock = threading.Lock()
rx_queue = queue.Queue()

# ---- SF RPC 定数 ----
SF_CMD_READ   = 0x02
SF_CMD_WRITE  = 0x03
SF_CMD_LS     = 0x05
SF_CMD_MKDIR  = 0x06
SF_CMD_RMDIR  = 0x07
SF_CMD_UNLINK = 0x08
SF_CMD_RENAME = 0x09
SF_CMD_GETSIZE = 0x0A
SF_CMD_READ_STREAM = 0x0B
SF_CMD_WRITE_STREAM = 0x0C

SF_ERR_OK     = 0x00
SF_ERR_NOF    = 0x01
SF_ERR_IO     = 0x02
SF_ERR_ACC    = 0x03
SF_ERR_EXT    = 0x04

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

def handle_serialfs_read(path_str):
    """ホスト上のファイルを読み取って返す"""
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    
    try:
        with open(target_path, "rb") as f:
            data = f.read()
            err = SF_ERR_OK
    except Exception as e:
        print(f"[SerialFS] Read error: {target_path} - {e}")
        data = b''
        err = SF_ERR_NOF
        
    print(f"[SerialFS] Sending read response, {len(data)} bytes (err={err})")
    _send_data_resp(err, data)

def _send_data_resp(err, data):
    sz_orig = len(data) if data else 0
    if data:
        encoded_data = lzss_encode(data)
    else:
        encoded_data = b''
        
    packet_len = len(encoded_data)
    if data:
        # 展開後のサイズ(4bytes)をパケット先頭に付与するため、実際のパケットサイズは +4
        packet_len += 4

    resp = bytearray([0x06, ord('S'), ord('F'), err])
    resp.append(packet_len & 0xFF)
    resp.append((packet_len >> 8) & 0xFF)
    resp.append((packet_len >> 16) & 0xFF)
    resp.append((packet_len >> 24) & 0xFF)
    
    # ヘッダ用Csum (8bit)
    csum = err
    csum += (packet_len & 0xFF) + ((packet_len >> 8) & 0xFF) + ((packet_len >> 16) & 0xFF) + ((packet_len >> 24) & 0xFF)
    resp.append(csum % 256)
    
    if data:
        payload = bytearray()
        payload.append(sz_orig & 0xFF)
        payload.append((sz_orig >> 8) & 0xFF)
        payload.append((sz_orig >> 16) & 0xFF)
        payload.append((sz_orig >> 24) & 0xFF)
        payload.extend(encoded_data)
        
        # ペイロード本体とその後ろに2バイトのCRC16を付加
        resp.extend(payload)
        crc = crc16(payload)
        resp.append(crc & 0xFF)
        resp.append((crc >> 8) & 0xFF)
        
    _pipe_write_bytes(resp)

def _send_simple_resp(err):
    _send_data_resp(err, None)

def handle_serialfs_rename(old_path_str, new_path_str):
    if old_path_str.startswith('/'): old_path_str = old_path_str[1:]
    if new_path_str.startswith('/'): new_path_str = new_path_str[1:]
    old_target = os.path.join(HOST_ROOT_DIR, os.path.normpath(old_path_str))
    new_target = os.path.join(HOST_ROOT_DIR, os.path.normpath(new_path_str))
    try:
        os.rename(old_target, new_target)
        _send_simple_resp(SF_ERR_OK)
    except FileNotFoundError:
        print(f"[SerialFS] RENAME not found: {old_target}")
        _send_simple_resp(SF_ERR_NOF)
    except FileExistsError:
        print(f"[SerialFS] RENAME exists: {new_target}")
        _send_simple_resp(SF_ERR_EXT)
    except Exception as e:
        print(f"[SerialFS] RENAME error: {old_target} -> {new_target} - {e}")
        _send_simple_resp(SF_ERR_IO)

def handle_serialfs_list(path_str):
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    
    try:
        entries = os.listdir(target_path)
        data = bytearray()
        for ent in entries:
            full = os.path.join(target_path, ent)
            is_dir = os.path.isdir(full)
            sz = os.path.getsize(full) if not is_dir else 0
            name_bytes = ent.encode('utf-8', errors='ignore')
            if len(name_bytes) > 255: name_bytes = name_bytes[:255]
            
            data.append(2 if is_dir else 1)
            data.append(sz & 0xFF)
            data.append((sz >> 8) & 0xFF)
            data.append((sz >> 16) & 0xFF)
            data.append((sz >> 24) & 0xFF)
            data.append(len(name_bytes))
            data.extend(name_bytes)
        err = SF_ERR_OK
    except Exception as e:
        print(f"[SerialFS] List error: {target_path} - {e}")
        data = bytearray()
        err = SF_ERR_NOF
        
    _send_data_resp(err, bytes(data))

def handle_serialfs_write(path_str, data_bytes):
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    try:
        with open(target_path, 'wb') as f:
            f.write(data_bytes)
        _send_simple_resp(SF_ERR_OK)
    except Exception as e:
        print(f"[SerialFS] Write error: {target_path} - {e}")
        _send_simple_resp(SF_ERR_IO)

def handle_serialfs_mkdir(path_str):
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    try:
        os.mkdir(target_path)
        _send_simple_resp(SF_ERR_OK)
    except FileExistsError:
        _send_simple_resp(SF_ERR_EXT)
    except Exception as e:
        print(f"[SerialFS] Mkdir error: {target_path} - {e}")
        _send_simple_resp(SF_ERR_ACC)

def handle_serialfs_rmdir(path_str):
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    try:
        os.rmdir(target_path)
        _send_simple_resp(SF_ERR_OK)
    except Exception as e:
        print(f"[SerialFS] Rmdir error: {target_path} - {e}")
        _send_simple_resp(SF_ERR_ACC)

def handle_serialfs_unlink(path_str):
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    try:
        os.remove(target_path)
        _send_simple_resp(SF_ERR_OK)
    except Exception as e:
        print(f"[SerialFS] Unlink error: {target_path} - {e}")
        _send_simple_resp(SF_ERR_NOF)

def handle_serialfs_getsize(path_str):
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    try:
        sz = os.path.getsize(target_path)
        data = bytearray([sz & 0xFF, (sz >> 8) & 0xFF, (sz >> 16) & 0xFF, (sz >> 24) & 0xFF])
        _send_data_resp(SF_ERR_OK, data)
    except FileNotFoundError:
        _send_simple_resp(SF_ERR_NOF)
    except Exception as e:
        print(f"[SerialFS] GetSize error: {target_path} - {e}")
        _send_simple_resp(SF_ERR_IO)

def handle_serialfs_read_stream(path_str, offset, size):
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    try:
        with open(target_path, 'rb') as f:
            f.seek(offset)
            data = f.read(size)
        _send_data_resp(SF_ERR_OK, data)
    except FileNotFoundError:
        _send_simple_resp(SF_ERR_NOF)
    except Exception as e:
        print(f"[SerialFS] ReadStream error: {target_path} - {e}")
        _send_simple_resp(SF_ERR_IO)

def handle_serialfs_write_stream(path_str, offset, data_bytes):
    if path_str.startswith('/'): path_str = path_str[1:]
    target_path = os.path.join(HOST_ROOT_DIR, os.path.normpath(path_str))
    try:
        # 存在しなければ新規作成が必要だが、r+bモードはファイルが無いとエラーになるので
        # w+b や aモードなど柔軟に扱うか、os.path.exists で分岐
        mode = 'r+b' if os.path.exists(target_path) else 'w+b'
        with open(target_path, mode) as f:
            f.seek(offset)
            f.write(data_bytes)
        _send_simple_resp(SF_ERR_OK)
    except Exception as e:
        print(f"[SerialFS] WriteStream error: {target_path} - {e}")
        _send_simple_resp(SF_ERR_IO)

def pipe_reader_thread():
    """パイプ受信監視・マルチプレクサスレッド"""
    global pipe_handle
    connect_to_pipe()
    
    state = 0
    cmd = 0
    payload_len = 0
    payload_buf = bytearray()
    
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
            
            # SerialFS RPC parser state machine
            if state == 0:
                if b == 0x05: state = 1  # ENQ
                else: rx_queue.put(b)    # 通常のコンソール表示
            elif state == 1:
                state = 2 if b == ord('S') else 0
            elif state == 2:
                state = 3 if b == ord('F') else 0
            elif state == 3:
                cmd = b
                print(f"[SerialFS] Magic received, CMD: {cmd}")
                payload_buf = bytearray()
                state = 4
            elif state == 4: payload_len = b; state = 5
            elif state == 5: payload_len |= (b << 8); state = 6
            elif state == 6: payload_len |= (b << 16); state = 7
            elif state == 7: payload_len |= (b << 24); state = 8 if payload_len > 0 else 9
            elif state == 8:
                payload_buf.append(b)
                if len(payload_buf) >= payload_len: state = 9
            elif state == 9:
                # Checksum validation (skipped for simplicity in parser)
                csum_rx = b
                if cmd == SF_CMD_READ:
                    path_str = payload_buf.decode('utf-8', errors='ignore')
                    print(f"[SerialFS] READ req: {path_str}")
                    handle_serialfs_read(path_str)
                elif cmd == SF_CMD_LS:
                    path_str = payload_buf.decode('utf-8', errors='ignore')
                    print(f"[SerialFS] LIST req: {path_str}")
                    handle_serialfs_list(path_str)
                elif cmd == SF_CMD_MKDIR:
                    path_str = payload_buf.decode('utf-8', errors='ignore')
                    print(f"[SerialFS] MKDIR req: {path_str}")
                    handle_serialfs_mkdir(path_str)
                elif cmd == SF_CMD_RMDIR:
                    path_str = payload_buf.decode('utf-8', errors='ignore')
                    print(f"[SerialFS] RMDIR req: {path_str}")
                    handle_serialfs_rmdir(path_str)
                elif cmd == SF_CMD_UNLINK:
                    path_str = payload_buf.decode('utf-8', errors='ignore')
                    print(f"[SerialFS] UNLINK req: {path_str}")
                    handle_serialfs_unlink(path_str)
                elif cmd == SF_CMD_WRITE:
                    # write payload: path_len (1 byte) + path_str + data
                    try:
                        if len(payload_buf) < 1:
                            raise ValueError("Payload missing path_len")
                        p_len = payload_buf[0]
                        if len(payload_buf) < 1 + p_len:
                            raise ValueError("Payload missing path_str")
                        path_str = payload_buf[1 : 1+p_len].decode('utf-8', errors='ignore')
                        data_bytes = payload_buf[1+p_len:]
                        
                        print(f"[SerialFS] WRITE req: {path_str} ({len(data_bytes)} bytes)")
                        handle_serialfs_write(path_str, data_bytes)
                    except ValueError as e:
                        print(f"[SerialFS] WRITE payload invalid: {e}")
                        _send_simple_resp(SF_ERR_IO)
                elif cmd == SF_CMD_RENAME:
                    # rename payload: old_len(1) + old_str + new_len(1) + new_str
                    try:
                        if len(payload_buf) < 2:
                            raise ValueError("Payload missing lengths")
                        old_len = payload_buf[0]
                        if len(payload_buf) < 1 + old_len + 1:
                            raise ValueError("Payload missing old_str or new_len")
                        old_path = payload_buf[1 : 1+old_len].decode('utf-8', errors='ignore')
                        new_len = payload_buf[1+old_len]
                        if len(payload_buf) < 1 + old_len + 1 + new_len:
                            raise ValueError("Payload missing new_str")
                        new_path = payload_buf[1+old_len+1 : 1+old_len+1+new_len].decode('utf-8', errors='ignore')
                        
                        print(f"[SerialFS] RENAME req: {old_path} -> {new_path}")
                        handle_serialfs_rename(old_path, new_path)
                    except ValueError as e:
                        print(f"[SerialFS] RENAME payload invalid: {e}")
                        _send_simple_resp(SF_ERR_IO)
                elif cmd == SF_CMD_GETSIZE:
                    # getsize payload: path_str
                    path_str = payload_buf.decode('utf-8', errors='ignore')
                    print(f"[SerialFS] GETSIZE req: {path_str}")
                    handle_serialfs_getsize(path_str)
                elif cmd == SF_CMD_READ_STREAM:
                    # read_stream payload: offset(4) + size(4) + path_str
                    try:
                        if len(payload_buf) < 8:
                            raise ValueError("Payload missing offset/size")
                        offset = payload_buf[0] | (payload_buf[1]<<8) | (payload_buf[2]<<16) | (payload_buf[3]<<24)
                        size   = payload_buf[4] | (payload_buf[5]<<8) | (payload_buf[6]<<16) | (payload_buf[7]<<24)
                        path_str = payload_buf[8:].decode('utf-8', errors='ignore')
                        print(f"[SerialFS] READ_STREAM req: {path_str} (offset={offset}, size={size})")
                        handle_serialfs_read_stream(path_str, offset, size)
                    except ValueError as e:
                        print(f"[SerialFS] READ_STREAM payload invalid: {e}")
                        _send_simple_resp(SF_ERR_IO)
                elif cmd == SF_CMD_WRITE_STREAM:
                    # write_stream payload: offset(4) + path_len(1) + path_str + data
                    try:
                        if len(payload_buf) < 5:
                            raise ValueError("Payload missing offset/path_len")
                        offset = payload_buf[0] | (payload_buf[1]<<8) | (payload_buf[2]<<16) | (payload_buf[3]<<24)
                        p_len = payload_buf[4]
                        if len(payload_buf) < 5 + p_len:
                            raise ValueError("Payload missing path_str")
                        path_str = payload_buf[5 : 5+p_len].decode('utf-8', errors='ignore')
                        data_bytes = payload_buf[5+p_len:]
                        print(f"[SerialFS] WRITE_STREAM req: {path_str} (offset={offset}, size={len(data_bytes)})")
                        handle_serialfs_write_stream(path_str, offset, data_bytes)
                    except ValueError as e:
                        print(f"[SerialFS] WRITE_STREAM payload invalid: {e}")
                        _send_simple_resp(SF_ERR_IO)
                else:
                    print(f"[SerialFS] Unknown CMD {cmd}")
                state = 0
                
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
            # print(f"[HTTP] Cmd: {cmd}")
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
    print(f"OS32 HTTP & SerialFS Server running on port {HTTP_PORT}...")
    
    t = threading.Thread(target=pipe_reader_thread, daemon=True)
    t.start()
    
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server...")
        httpd.server_close()

if __name__ == '__main__':
    run_server()
