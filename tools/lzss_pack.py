import sys
import struct
import os

N = 4096  # リングバッファのサイズ
F = 18    # 最大一致長 (2 + 16)
THRESHOLD = 2

def encode(in_data):
    out_data = bytearray()
    # 先頭4バイトにオリジナルサイズを含有
    out_data.extend(struct.pack('<I', len(in_data)))
    
    text_buf = bytearray(N + F - 1)
    for i in range(N):
        text_buf[i] = 0x20  # Lempel-Ziv の古典的初期値

    in_len = len(in_data)
    src_p = 0
    r = N - F

    flags = 0
    flag_pos = 0
    code_buf = bytearray(17)
    code_buf_ptr = 1

    while src_p < in_len:
        # Longest match 検索
        match_pos = 0
        match_len = 0
        
        # 検索窓は r の前 N文字分
        for i in range(1, N + 1):
            cmp_p = (r - i) % N
            l = 0
            while l < F and src_p + l < in_len and text_buf[(cmp_p + l) % N] == in_data[src_p + l]:
                l += 1
            if l > match_len:
                match_len = l
                match_pos = cmp_p
                if match_len == F:
                    break
        
        if match_len <= THRESHOLD:
            match_len = 1
            flags |= (1 << flag_pos)
            code_buf[code_buf_ptr] = in_data[src_p]
            code_buf_ptr += 1
        else:
            code_buf[code_buf_ptr] = match_pos & 0xFF
            code_buf_ptr += 1
            code_buf[code_buf_ptr] = ((match_pos >> 4) & 0xF0) | (match_len - (THRESHOLD + 1))
            code_buf_ptr += 1

        flag_pos += 1
        if flag_pos == 8:
            code_buf[0] = flags
            out_data.extend(code_buf[:code_buf_ptr])
            flags = 0
            flag_pos = 0
            code_buf_ptr = 1

        for i in range(match_len):
            if src_p < in_len:
                text_buf[r] = in_data[src_p]
                r = (r + 1) % N
                src_p += 1

    if flag_pos > 0:
        code_buf[0] = flags
        out_data.extend(code_buf[:code_buf_ptr])

    return out_data

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python lzss_pack.py <in_file> <out_file>")
        sys.exit(1)
        
    in_file = sys.argv[1]
    out_file = sys.argv[2]
    
    with open(in_file, 'rb') as f:
        in_data = f.read()
        
    out_data = encode(in_data)
    
    with open(out_file, 'wb') as f:
        f.write(out_data)
        
    print(f"LZSS: Compressed {len(in_data)} bytes -> {len(out_data)} bytes (ratio: {len(out_data)/len(in_data)*100:.1f}%)")
