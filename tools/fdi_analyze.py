#!/usr/bin/env python3
"""FDI イメージの IPL ブートコードを解析し、INT 1Bh 呼び出しパラメータを特定する"""
import struct, sys

fdi_path = sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/os32/dos5_1.fdi"

with open(fdi_path, "rb") as f:
    data = f.read(4096 + 1024)  # header + IPL

# FDI header
hdr_size  = struct.unpack_from("<I", data, 8)[0]
sect_size = struct.unpack_from("<I", data, 16)[0]
spt       = struct.unpack_from("<I", data, 20)[0]
heads     = struct.unpack_from("<I", data, 24)[0]
cyls      = struct.unpack_from("<I", data, 28)[0]
n_code = 0; tmp = sect_size
while tmp > 128 and n_code < 8: tmp >>= 1; n_code += 1

print(f"=== FDI: hdr={hdr_size} C={cyls} H={heads} S={spt} BPS={sect_size} N={n_code} ===")

# IPL
ipl = data[hdr_size : hdr_size + 1024]

# BPB
bps_bpb = struct.unpack_from("<H", ipl, 0x0B)[0]
spt_bpb = struct.unpack_from("<H", ipl, 0x18)[0]
hd_bpb  = struct.unpack_from("<H", ipl, 0x1A)[0]
print(f"BPB: BPS={bps_bpb} SPT={spt_bpb} Heads={hd_bpb}")

# INT 1Bh (CD 1B) locations
print("\n=== INT 1Bh (CD 1B) in IPL ===")
for i in range(len(ipl) - 1):
    if ipl[i] == 0xCD and ipl[i+1] == 0x1B:
        s = max(0, i - 20)
        e = min(len(ipl), i + 10)
        ctx = " ".join(f"{b:02X}" for b in ipl[s:e])
        print(f"  offset 0x{i:03X}: [{ctx}]")

# Full hex dump from 0x3C (JMP target) to 0x180
print("\n=== IPL code hex dump (0x3C - 0x180) ===")
for i in range(0x3C, min(0x180, len(ipl)), 16):
    h = " ".join(f"{b:02X}" for b in ipl[i:i+16])
    a = "".join(chr(b) if 32 <= b < 127 else "." for b in ipl[i:i+16])
    print(f"  {i:04X}: {h}  {a}")

# Search for MOV CH, or references to 0564
print("\n=== Patterns: B5 xx (MOV CH,imm8) ===")
for i in range(len(ipl) - 1):
    if ipl[i] == 0xB5:  # MOV CH, imm8
        print(f"  0x{i:03X}: MOV CH, 0x{ipl[i+1]:02X}")

# Search for references to segment 0000 with offset 0564+
print("\n=== References to 0564h area ===")
for i in range(len(ipl) - 3):
    val16 = struct.unpack_from("<H", ipl, i)[0]
    if 0x0560 <= val16 <= 0x056F:
        ctx = " ".join(f"{b:02X}" for b in ipl[max(0,i-4):min(len(ipl),i+6)])
        print(f"  0x{i:03X}: word 0x{val16:04X} [{ctx}]")
