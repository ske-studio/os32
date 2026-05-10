#!/bin/bash
# FreeDOS(98) NEC98 Build Script (Hybrid: WSL + cmd.exe)
# Usage: ./build98.sh [debug]
set -e

FDROOT="/mnt/c/WATCOM/src/os32/tools/fdkernel"
NEC98="$FDROOT/nec98"

echo "====== FreeDOS(98) NEC98 Build ======"

# Step 1: Build host tools (Linux native)
echo "=== HOST TOOLS (Linux GCC) ==="
gcc -o "$NEC98/utils/exeflat" -I"$FDROOT/hdr" -DFOR_NEC98 "$NEC98/utils/exeflat.c" 2>/dev/null
gcc -o "$NEC98/utils/patchobj" -I"$FDROOT/hdr" -DBUILD_UTILS "$FDROOT/utils/patchobj.c" 2>/dev/null
echo "  exeflat: OK"
echo "  patchobj: OK"

# Step 2: Build kernel via cmd.exe (OpenWatcom)
echo ""
echo "=== KERNEL (OpenWatcom via cmd.exe) ==="

# Set env and run sub-builds
ENVSETUP='set WATCOM=C:\WATCOM& set PATH=C:\WATCOM\binnt64;%PATH%& set INCLUDE=C:\WATCOM\h& set EDPATH=C:\WATCOM\eddat& set COMPILER=owwin& set XCPU=86& set XFAT=16& set XNASM=nasm& set XLINK=wlink'

if [ "$1" = "debug" ]; then
    ENVSETUP="$ENVSETUP& set ALLCFLAGS=-DDEBUG -wcd303"
fi

# Drivers
echo "  [DRIVERS]"
cmd.exe /C "$ENVSETUP& cd /D C:\WATCOM\src\os32\tools\fdkernel\nec98\drivers& wmake -ms -h -e -f makefile.wc production" 2>&1 | tail -1

# Lib
echo "  [LIB]"
touch "$NEC98/lib/libm.lib" 2>/dev/null || true

# Kernel compile + link
echo "  [KERNEL]"
cmd.exe /C "$ENVSETUP& cd /D C:\WATCOM\src\os32\tools\fdkernel\nec98\kernel& wmake -ms -h -e -f makefile.wc kernel.exe" 2>&1 | tail -3

# Step 3: Post-process with Linux tools
echo ""
echo "=== POST-PROCESS (exeflat) ==="
cd "$NEC98/kernel"

"$NEC98/utils/exeflat" kernel.exe kernel.sys 0x60 -S0x10 -S0x78 -S0x79
"$NEC98/utils/exeflat" kernel.exe kernel.raw 0x60 -S0x10 -S0x78 -S0x79

# Copy to bin
mkdir -p "$NEC98/bin"
cp kernel.sys "$NEC98/bin/"
cp kernel.sys "$NEC98/bin/KWC8616.sys"
cp kernel.map "$NEC98/bin/KWC8616.map" 2>/dev/null || true

echo ""
echo "====== Build Complete! ======"
ls -la "$NEC98/bin/"*.sys 2>/dev/null
