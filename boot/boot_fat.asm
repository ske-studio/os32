;; ============================================================
;; boot_fat.asm — PC-98 FAT12 IPLブートセクタ
;;
;; PC-98 2HD 1024B/sector FAT12フロッピーからLOADER.BINを読み込み、
;; 0000:8000h にロードしてジャンプする。
;;
;; NP21/Wでは 1FC0:0000 にロードされる (物理 0x1FC00)
;; BPBはmkfat12.pyによって上書きされる場合がある
;;
;; FAT12レイアウト (PC-98 2HD):
;;   セクタ0:   ブートセクタ (本コード)
;;   セクタ1-2: FAT1 (2セクタ)
;;   セクタ3-4: FAT2
;;   セクタ5-10: ルートディレクトリ (192エントリ)
;;   セクタ11〜: データ領域 (クラスタ2から)
;;
;; LBA→CHS (spt=8, heads=2):
;;   cylinder = LBA / 16
;;   head     = (LBA / 8) & 1
;;   sector   = (LBA % 8) + 1
;; ============================================================

cpu 8086


DA_UA       EQU     090h        ;; DA/UA: 1MB FDD ユニット0
LOAD_DEST   EQU     8000h       ;; LOADER.BIN ロード先
FAT_BUF     EQU     6000h       ;; FAT/RootDir一時バッファ
ROOT_START  EQU     5           ;; ルートDir開始LBA
ROOT_SECTS  EQU     6           ;; ルートDirセクタ数
FAT_START   EQU     1           ;; FAT開始LBA
DATA_START  EQU     11          ;; データ領域開始LBA

section .text
        org 0x0

global start
start:
        jmp     short boot_main
        nop

;; ============================================================
;; BPB (BIOS Parameter Block) — 0x03〜0x23
;; mkfat12.py が正しい値で上書きする
;; ============================================================
bpb_oem:        db 'OS32IPL '      ;; 0x03: OEM名
bpb_bps:        dw 0400h           ;; 0x0B: bytes/sector = 1024
bpb_spc:        db 01h             ;; 0x0D: sectors/cluster = 1
bpb_resv:       dw 0001h           ;; 0x0E: reserved = 1
bpb_nfats:      db 02h             ;; 0x10: FAT数 = 2
bpb_rootcnt:    dw 00C0h           ;; 0x11: root entries = 192
bpb_totsect:    dw 04D0h           ;; 0x13: total sectors = 1232
bpb_media:      db 0FEh            ;; 0x15: media = PC-98 2HD
bpb_fatsz:      dw 0002h           ;; 0x16: FAT size = 2
bpb_spt:        dw 0008h           ;; 0x18: sectors/track = 8
bpb_heads:      dw 0002h           ;; 0x1A: heads = 2
bpb_hidden:     dd 0               ;; 0x1C: hidden sectors
bpb_totsect32:  dd 0               ;; 0x20: total sectors 32bit
;; (ここまで0x24 = 36バイト)

;; ============================================================
;; ブートコード本体
;; ============================================================
boot_main:
        ;; セグメント初期化
        cli
        push    cs
        pop     ds              ;; DS = CS (BPBアクセス用)
        xor     ax, ax
        mov     es, ax          ;; ES = 0 (データ読込先)
        mov     ss, ax
        mov     sp, 7C00h
        sti

        ;; 起動メッセージ表示
        call    show_boot_msg

        ;; ============================================================
        ;; ルートディレクトリを0:6000にロード (6セクタ)
        ;; ============================================================
        mov     cx, ROOT_SECTS
        mov     ax, ROOT_START  ;; LBA = 5
        mov     bp, FAT_BUF    ;; ES:BP = 0:6000
.rd_loop:
        push    cx
        push    ax
        call    read_sector
        pop     ax
        pop     cx
        inc     ax
        add     bp, 0400h       ;; +1024
        loop    .rd_loop

        ;; ============================================================
        ;; ルートDirから "LOADER  BIN" を検索
        ;; ============================================================
        mov     di, FAT_BUF     ;; ES:DI = 0:6000
        mov     cx, 192
.scan_dir:
        ;; エントリ先頭バイト確認
        mov     al, es:[di]
        cmp     al, 0           ;; 終端?
        je      .no_loader
        cmp     al, 0E5h        ;; 削除済み?
        je      .scan_next

        ;; 11バイト比較
        push    cx
        push    di
        mov     si, loader_name
        mov     cx, 11
        repe    cmpsb
        pop     di
        pop     cx
        je      .found_loader

.scan_next:
        add     di, 32          ;; 次のエントリ
        loop    .scan_dir

.no_loader:
        ;; LOADER.BIN見つからない → エラー
        mov     si, msg_noldr
        call    show_error
        jmp     disk_error.halt

.found_loader:
        ;; 開始クラスタ取得 (DirEntry 0x1A)
        mov     ax, es:[di + 1Ah]
        mov     word [var_cluster], ax

        ;; ============================================================
        ;; FATテーブルを0:6000にロード (2セクタ = 2048B)
        ;; ルートDirバッファを上書き (もう不要)
        ;; ============================================================
        mov     ax, FAT_START   ;; LBA = 1
        mov     bp, FAT_BUF    ;; 0:6000
        call    read_sector
        mov     ax, FAT_START + 1  ;; LBA = 2
        mov     bp, FAT_BUF + 0400h ;; 0:6400
        call    read_sector

        ;; ============================================================
        ;; FATチェーンに沿ってLOADER.BINを0:8000にロード
        ;; ============================================================
        mov     bp, LOAD_DEST   ;; ES:BP = 0:8000
        mov     ax, word [var_cluster]

.load_chain:
        ;; クラスタ→LBA: LBA = DATA_START + (cluster - 2)
        push    ax
        sub     ax, 2
        add     ax, DATA_START
        call    read_sector
        pop     ax

        ;; 次の宛先
        add     bp, 0400h       ;; +1024

        ;; FAT12で次のクラスタを取得
        call    fat12_next      ;; AX = next cluster

        ;; EOC (>= 0xFF8) ?
        cmp     ax, 0FF8h
        jb      .load_chain

        ;; ============================================================
        ;; LOADER.BINにジャンプ
        ;; ============================================================
        db      0EAh            ;; far jmp opcode
        dw      LOAD_DEST       ;; = 0x8000
        dw      0000h           ;; segment = 0x0000

;; ============================================================
;; read_sector — 1セクタ(1024B)をES:BPに読み込む
;; 入力: AX = LBA, ES:BP = 行き先
;; 破壊: AX, BX, CX, DX
;; ============================================================
read_sector:
        push    ax
        mov     bx, ax          ;; BX = LBA (保存)

        ;; sector = (LBA % 8) + 1
        and     al, 07h
        inc     al
        mov     dl, al          ;; DL = sector (1-based)

        ;; head = (LBA / 8) & 1
        mov     ax, bx
        mov     cl, 3
        shr     ax, cl          ;; AX = LBA / 8
        mov     dh, al
        and     dh, 1           ;; DH = head

        ;; cylinder = LBA / 16
        shr     ax, 1           ;; AX = LBA / 16
        mov     cl, al          ;; CL = cylinder

        ;; INT 1Bh: FDD読み込み
        mov     ah, 76h         ;; SEEK+RETRY+READ
        mov     al, DA_UA       ;; 0x90
        mov     bx, 0400h       ;; 1024 bytes
        mov     ch, 03h         ;; sector size = 1024B
        int     1Bh

        pop     ax
        jc      disk_error
        ret

;; ============================================================
;; fat12_next — FAT12チェーンの次のクラスタを取得
;; 入力: AX = 現在のクラスタ番号
;; 出力: AX = 次のクラスタ番号
;; FATデータは 0:6000h にロード済み
;; ============================================================
fat12_next:
        push    bx
        push    dx

        ;; = cluster + cluster/2 (1.5バイト/エントリ)
        mov     bx, ax
        shr     bx, 1
        add     bx, ax          ;; BX = byte in FAT

        ;; 16ビットロード from ES:[FAT_BUF + BX]
        add     bx, FAT_BUF
        mov     dx, es:[bx]

        ;; 奇数/偶数で上位/下位4bit選択
        test    ax, 1
        jnz     .odd
        and     dx, 0FFFh
        jmp     .done
.odd:
        mov     cl, 4
        shr     dx, cl
        and     dx, 0FFFh
.done:
        mov     ax, dx

        pop     dx
        pop     bx
        ret

;; ============================================================
;; disk_error — ディスクエラー表示
;; ============================================================
disk_error:
        mov     si, msg_derr
        call    show_error
.halt:
        hlt
        jmp     .halt

;; ============================================================
;; show_boot_msg — 起動メッセージをTVRAMに表示
;; ============================================================
show_boot_msg:
        push    es
        mov     ax, 0A000h
        mov     es, ax
        xor     di, di          ;; 0行目先頭
        mov     si, msg_boot
        call    print_str
        pop     es
        ret

;; ============================================================
;; show_error — エラーメッセージをTVRAMの2行目に表示
;; ============================================================
show_error:
        push    es
        mov     ax, 0A000h
        mov     es, ax
        mov     di, 160         ;; 2行目 (80文字×2バイト)
        call    print_str
        pop     es
        ret

;; ============================================================
;; print_str — テキストVRAMに文字列書き込み
;; 入力: ES = 0xA000, DI = TVRAMオフセット, DS:SI = 文字列
;; ============================================================
print_str:
        lodsb
        or      al, al
        jz      .done
        mov     ah, 0
        mov     es:[di], ax
        push    di
        add     di, 2000h
        mov     byte es:[di], 0E1h
        pop     di
        add     di, 2
        jmp     print_str
.done:
        ret

;; ============================================================
;; データ領域
;; ============================================================
loader_name:    db 'LOADER  BIN'   ;; 8.3形式ファイル名
msg_boot:       db 'PC-98 OS32 FAT12 Boot', 0
msg_noldr:      db 'No LOADER.BIN!', 0
msg_derr:       db 'Disk Error!', 0

var_cluster:    dw 0               ;; 一時変数: クラスタ番号

        times 1022 - ($ - $$) db 0
        db      055h, 0AAh

