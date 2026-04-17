#!/bin/bash
# ======================================================================
# nhd_deploy.sh — NHDイメージのext2にファイルをコピー (sudo不要)
#
# debugfs (e2fsprogs) を使ってext2を直接操作する。
# NHDヘッダ(512B)をddでスキップし、一時RAWファイル経由で操作する。
#
# 使い方:
#   bash nhd_deploy.sh copy <src_file> [dest_name]
#   bash nhd_deploy.sh ls [path]
# ======================================================================

set -e

NP21W_DIR="${NP21W_DIR:-/tmp/np21w}"
NHD_FILE="$NP21W_DIR/os32.nhd"
TMP_RAW="/tmp/os32_ext2.raw"
TMP_HDR="/tmp/os32_nhd_header.bin"

# NHDからext2 RAWを抽出 (512Bヘッダをスキップ)
extract_raw() {
    dd if="$NHD_FILE" of="$TMP_RAW" bs=512 skip=1 status=progress 2>&1
}

# 変更済みRAWをNHDに書き戻す
write_back() {
    # ヘッダ保存 (512B)
    dd if="$NHD_FILE" of="$TMP_HDR" bs=512 count=1 2>/dev/null
    # ヘッダ + 変更済みRAW = 新NHD
    cat "$TMP_HDR" "$TMP_RAW" > "${NHD_FILE}.tmp"
    mv "${NHD_FILE}.tmp" "$NHD_FILE"
    rm -f "$TMP_HDR"
}

cleanup() {
    rm -f "$TMP_RAW" "$TMP_HDR"
}

case "${1:-help}" in
    copy)
        SRC="$2"
        DEST="${3:-$(basename "$2")}"
        if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
            echo "Usage: $0 copy <src_file> [dest_name]"
            exit 1
        fi
        echo "=== Extracting ext2 from NHD ==="
        extract_raw
        echo "=== Writing $SRC -> /$DEST ==="
        debugfs -w -R "write $SRC $DEST" "$TMP_RAW" 2>&1
        echo "=== Writing back to NHD ==="
        write_back
        echo "Done: $SRC -> $DEST"
        cleanup
        ;;
    ls)
        DIR="${2:-/}"
        extract_raw
        debugfs -R "ls -l $DIR" "$TMP_RAW" 2>&1
        cleanup
        ;;
    cat)
        [ -z "$2" ] && { echo "Usage: $0 cat <file>"; exit 1; }
        extract_raw
        debugfs -R "cat $2" "$TMP_RAW" 2>&1
        cleanup
        ;;
    rm)
        [ -z "$2" ] && { echo "Usage: $0 rm <file>"; exit 1; }
        extract_raw
        debugfs -w -R "rm $2" "$TMP_RAW" 2>&1
        write_back
        echo "Removed: $2"
        cleanup
        ;;
    help|*)
        echo "NHD ext2 Deploy Tool (sudo不要)"
        echo "Usage: $0 {copy|ls|cat|rm}"
        echo "  copy <src> [dest]  — ファイルコピー"
        echo "  ls [path]          — ファイル一覧"
        echo "  cat <file>         — ファイル表示"
        echo "  rm <file>          — ファイル削除"
        echo ""
        echo "注意: NP21/Wを先に停止してください"
        ;;
esac
