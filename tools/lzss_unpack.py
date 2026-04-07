import sys
import struct

def decode(in_file, out_file):
    with open(in_file, 'rb') as f:
        in_data = f.read()

    orig_size = struct.unpack('<I', in_data[0:4])[0]
    out_data = bytearray(orig_size)
    
    src_p = 4
    dst_p = 0
    N = 4096
    text_buf = bytearray([0x20] * N)
    r = N - 18

    while src_p < len(in_data) and dst_p < orig_size:
        flags = in_data[src_p]
        src_p += 1
        
        for _ in range(8):
            if flags & 1:
                if src_p < len(in_data) and dst_p < orig_size:
                    c = in_data[src_p]
                    src_p += 1
                    out_data[dst_p] = c
                    dst_p += 1
                    text_buf[r] = c
                    r = (r + 1) & (N - 1)
            else:
                if src_p + 1 < len(in_data):
                    j = in_data[src_p]
                    k = in_data[src_p + 1]
                    src_p += 2
                    
                    pos = j | ((k & 0xF0) << 4)
                    length = (k & 0x0F) + 3
                    
                    for _ in range(length):
                        if dst_p < orig_size:
                            c = text_buf[(pos + _) & (N - 1)]
                            out_data[dst_p] = c
                            dst_p += 1
                            text_buf[r] = c
                            r = (r + 1) & (N - 1)
            
            flags >>= 1
            if src_p >= len(in_data) or dst_p >= orig_size:
                break

    with open(out_file, 'wb') as f:
        f.write(out_data)

if __name__ == '__main__':
    decode(sys.argv[1], sys.argv[2])
