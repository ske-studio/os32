;; ============================================================
;; loader_fat_new.asm — FAT12対応 新ローダー (vmkernel.lz4)
;;
;; boot_fat.asmによって0000:8000hにロードされる
;;
;; 処理:
;;   1. FAT12からVMKRNL.LZ4を検索・メモリにロード
;;   2. A20ゲート有効化
;;   3. GDT設定 → プロテクトモード遷移
;;   4. PM後: リアルモードで読んだ断片を0x10000に統合
;;   5. VK32ヘッダ解析 + LZ4展開 → entry[i].load_addr
;;   6. メモリプローブ → カーネルにジャンプ
;;
;; VMKRNL.LZ4のメモリ配置 (リアルモード読み込み):
;;   Phase 1: 0:C000h〜0:FFFFh (16KB)
;;   Phase 2: 1000:0000h〜 (物理0x10000以降, 64KB毎にセグメント切替)
;;   PM移行後: 全データを 0x10000 に再配置
;;
;; PC-98 2HD FAT12:
;;   セクタ5-10: ルートDir (192エントリ)
;;   セクタ1-2:  FAT
;;   セクタ11〜: データ (クラスタ2から)
;; ============================================================

cpu 386


DA_UA       EQU     090h
FAT_BUF     EQU     6000h
ROOT_START  EQU     5
ROOT_SECTS  EQU     6
FAT_START   EQU     1
DATA_START  EQU     11
LOAD_SEG0   EQU     0000h       ;; Phase 1: セグメント0
LOAD_OFF0   EQU     0C000h      ;; Phase 1: 0:C000h から (16KB利用可能)
LOAD_SEG1   EQU     1000h       ;; Phase 2: セグメント0x1000 (物理0x10000)

;; LZ4デコーダ定数
LZ4_MINMATCH EQU    4

section .text


        org 8000h

global loader_start
loader_start:
        xor     ax, ax
        mov     ds, ax
        mov     ss, ax
        mov     sp, 7C00h

        ;; TVRAM表示
        mov     ax, 0A000h
        mov     es, ax
        mov     di, 160
        mov     si, msg_loader
        call    print16

        ;; ES = 0 に戻す
        xor     ax, ax
        mov     es, ax

        ;; ============================================================
        ;; ルートDirを0:6000にロード
        ;; ============================================================
        mov     cx, ROOT_SECTS
        mov     ax, ROOT_START
        mov     bp, FAT_BUF
.rd_loop:
        push    cx
        push    ax
        call    read_sect16
        pop     ax
        pop     cx
        inc     ax
        add     bp, 0400h
        loop    .rd_loop

        ;; ============================================================
        ;; VMKRNL.LZ4を検索 (8.3形式: "VMKRNL  LZ4")
        ;; ============================================================
        mov     di, FAT_BUF
        mov     cx, 192
.scan_kern:
        mov     al, es:[di]
        cmp     al, 0
        je      .no_kernel
        cmp     al, 0E5h
        je      .scan_knext

        push    cx
        push    di
        mov     si, kern_name
        mov     cx, 11
        repe    cmpsb
        pop     di
        pop     cx
        je      .found_kern

.scan_knext:
        add     di, 32
        loop    .scan_kern

.no_kernel:
        mov     ax, 0A000h
        mov     es, ax
        mov     di, 320
        mov     si, msg_nokernel
        call    print16
.halt16:
        hlt
        jmp     .halt16

.found_kern:
        ;; 開始クラスタとサイズ取得
        mov     ax, es:[di + 1Ah]
        mov     word [var_cluster], ax
        mov     ax, es:[di + 1Ch]
        mov     word [var_size_lo], ax
        mov     ax, es:[di + 1Eh]
        mov     word [var_size_hi], ax

        ;; ============================================================
        ;; FATテーブルを0:6000にロード (2セクタ)
        ;; ============================================================
        xor     ax, ax
        mov     es, ax
        mov     ax, FAT_START
        mov     bp, FAT_BUF
        call    read_sect16
        mov     ax, FAT_START + 1
        mov     bp, FAT_BUF + 0400h
        call    read_sect16

        ;; ============================================================
        ;; VMKRNL.LZ4をメモリにロード
        ;; 1000:0000h〜 (物理0x10000以降, 64KB毎にセグメント切替)
        ;; ============================================================
        mov     ax, word [var_cluster]
        mov     word [var_load_seg], 1000h
        xor     bp, bp

.load_kern:
        push    ax
        sub     ax, 2
        add     ax, DATA_START

        push    es
        mov     bx, word [var_load_seg]
        mov     es, bx
        call    read_sect16
        pop     es

        pop     ax

        add     bp, 0400h

        ;; 64KB境界チェック
        or      bp, bp
        jnz     .seg_ok

        ;; セグメント切替
        mov     bx, word [var_load_seg]
        add     bx, 1000h       ;; +64KB
        mov     word [var_load_seg], bx

.seg_ok:
        call    fat12_next16
        cmp     ax, 0FF8h
        jb      .load_kern

        ;; ============================================================
        ;; PM遷移
        ;; ============================================================
        push    es
        mov     ax, 0A000h
        mov     es, ax
        mov     di, 320
        mov     si, msg_pm
        call    print16
        pop     es

        ;; A20ゲート有効化
        mov     al, 3
        out     0F2h, al

        cli
        xor     al, al
        out     50h, al

        ;; GDTロード
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        add     eax, gdt
        mov     dword [gdtr_base], eax
        lgdt    [gdtr]

        mov     eax, cr0
        or      al, 1
        mov     cr0, eax

        ;; far jmp → PM
        db      066h
        db      0EAh
        dd      pm_entry32
        dw      0008h


;; ============================================================
;; 32bit PM コード
;; ============================================================

bits 32

pm_entry32:
        mov     eax, 10h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0009FFFCh

        ;; ファイルは既に 0x10000 (1000:0000h) 以降に直接配置されているため
        ;; Phase 1 / Phase 2 の統合コピー処理は不要

        ;; === VK32ヘッダ解析 + LZ4展開 ===
        ;; ファイル全体が 0x10000 に配置された状態
        mov     esi, 10000h
        cmp     dword [esi], 32334B56h  ;; VK32マジック
        jne     .bad_magic

        mov     ecx, [esi + 12]         ;; entry_count
        cmp     ecx, 4
        ja      .bad_magic

        ;; エントリループ
        xor     ebx, ebx               ;; エントリインデックス
.decomp_loop:
        cmp     ebx, ecx
        jge     .decomp_done

        ;; entries[ebx]: 16 + ebx*16 からのオフセット
        lea     edx, [esi + 16]         ;; entries ベース
        shl     ebx, 4                  ;; *16
        add     edx, ebx               ;; edx = &entries[ebx]
        shr     ebx, 4                  ;; ebx 復元

        ;; LZ4展開: boot_lz4_decode(src, csz, dst, raw_sz)
        ;; src = file_base + data_offset
        mov     eax, [edx + 8]          ;; data_offset
        add     eax, 10000h             ;; ファイルベースからの絶対アドレス

        push    dword [edx + 4]         ;; 第4引数: raw_size (dst_capacity)
        push    dword [edx + 0]         ;; 第3引数: load_addr (dst)
        push    dword [edx + 12]        ;; 第2引数: compressed_size
        push    eax                     ;; 第1引数: src

        call    pm_lz4_decode
        add     esp, 16

        ;; エラーチェック
        test    eax, eax
        js      .lz4_fail

        inc     ebx
        jmp     .decomp_loop

.decomp_done:
        ;; TVRAM: デコード完了
        mov     edi, 0A0000h + 480
        mov     esi, msg_ok32
        call    pm_print32

        ;; === メモリプロービング ===
        mov     esi, 00100000h
        mov     ecx, 1024

.probe_loop:
        mov     eax, [esi]
        mov     dword [esi], 0AA55AA55h
        cmp     dword [esi], 0AA55AA55h
        jne     .probe_done
        mov     dword [esi], 055AA55AAh
        cmp     dword [esi], 055AA55AAh
        jne     .probe_done
        mov     [esi], eax
        add     ecx, 512
        add     esi, 00080000h
        cmp     esi, 01000000h
        jb      .probe_loop

.probe_done:
        ;; === カーネルにジャンプ ===
        mov     eax, DA_UA
        push    eax             ;; boot_drive
        push    ecx             ;; mem_kb
        push    dword 0         ;; ダミーリターンアドレス

        db      0EAh
        dd      00100000h
        dw      0008h

.bad_magic:
        mov     edi, 0A0000h + 480
        mov     esi, msg_badmagic
        call    pm_print32
        jmp     pm_halt

.lz4_fail:
        mov     edi, 0A0000h + 480
        mov     esi, msg_lz4err
        call    pm_print32
        jmp     pm_halt


;; ============================================================
;; pm_lz4_decode — LZ4ブロックデコーダ (32bit PM, cdecl)
;;
;; int pm_lz4_decode(u8 *src, int csz, u8 *dst, int cap)
;; スタック: [ebp+8]=src, [ebp+12]=csz, [ebp+16]=dst, [ebp+20]=cap
;; 戻り値: EAX = 展開バイト数 (負=エラー)
;; ============================================================

pm_lz4_decode:
        push    ebp
        mov     ebp, esp
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     esi, [ebp+8]    ;; src (ip)
        mov     ecx, [ebp+12]   ;; compressed_size
        lea     ebx, [esi+ecx]  ;; ip_end
        mov     edi, [ebp+16]   ;; dst (op)
        mov     ecx, [ebp+20]   ;; cap
        lea     edx, [edi+ecx]  ;; op_end
        push    edi             ;; 保存: dst_start

.lz4_loop:
        cmp     esi, ebx
        jge     .lz4_end

        ;; トークン
        movzx   eax, byte [esi]
        inc     esi
        push    eax             ;; 保存: token

        ;; リテラル長
        shr     eax, 4
        and     eax, 0Fh
        cmp     eax, 15
        jne     .lz4_lit_copy
.lz4_lit_ext:
        cmp     esi, ebx
        jge     .lz4_err
        movzx   ecx, byte [esi]
        inc     esi
        add     eax, ecx
        cmp     ecx, 255
        je      .lz4_lit_ext

.lz4_lit_copy:
        ;; eax = lit_len
        mov     ecx, eax
        ;; 境界チェック省略 (ブートローダーなので信頼できるデータ)
        rep     movsb

        ;; 入力終端?
        cmp     esi, ebx
        jge     .lz4_end_pop

        ;; オフセット (2B LE)
        movzx   eax, word [esi]
        add     esi, 2
        test    eax, eax
        jz      .lz4_err_pop
        push    eax             ;; 保存: offset

        ;; マッチ長
        pop     eax             ;; offset 復元 → 後で使う
        push    eax             ;; 再保存

        ;; token の下位4bit
        mov     ecx, [esp+4]    ;; token (スタック上)
        and     ecx, 0Fh
        add     ecx, LZ4_MINMATCH
        cmp     ecx, 15 + LZ4_MINMATCH
        jne     .lz4_match_copy
.lz4_match_ext:
        cmp     esi, ebx
        jge     .lz4_err_pop2
        movzx   eax, byte [esi]
        inc     esi
        add     ecx, eax
        cmp     eax, 255
        je      .lz4_match_ext

.lz4_match_copy:
        ;; ecx = match_len, [esp] = offset
        pop     eax             ;; offset
        push    esi             ;; src 保存
        mov     esi, edi
        sub     esi, eax        ;; match_src = op - offset
        rep     movsb
        pop     esi             ;; src 復元

        ;; token 除去
        add     esp, 4
        jmp     .lz4_loop

.lz4_end_pop:
        add     esp, 4          ;; token 除去
.lz4_end:
        pop     eax             ;; dst_start
        sub     edi, eax        ;; 展開バイト数
        mov     eax, edi

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     ebp
        ret

.lz4_err_pop2:
        add     esp, 4          ;; offset
.lz4_err_pop:
        add     esp, 4          ;; token
.lz4_err:
        pop     eax             ;; dst_start (捨て)
        mov     eax, -1

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     ebp
        ret


;; ============================================================
;; pm_print32 — TVRAM表示 (32bit PM)
;; ============================================================

pm_print32:
        push    eax
.loop:
        mov     al, [esi]
        inc     esi
        or      al, al
        jz      .done
        mov     ah, 0
        mov     [edi], ax
        push    edi
        add     edi, 2000h
        mov     byte [edi], 0E1h
        pop     edi
        add     edi, 2
        jmp     .loop
.done:
        pop     eax
        ret

pm_halt:
        hlt
        jmp     pm_halt


bits 16

;; ============================================================
;; read_sect16 — 16ビットモード1セクタ読み込み
;; ============================================================
read_sect16:
        push    ax
        mov     bx, ax
        and     al, 07h
        inc     al
        mov     dl, al
        mov     ax, bx
        mov     cl, 3
        shr     ax, cl
        mov     dh, al
        and     dh, 1
        shr     ax, 1
        mov     cl, al
        mov     ah, 76h
        mov     al, DA_UA
        mov     bx, 0400h
        mov     ch, 03h
        int     1Bh
        pop     ax
        jc      disk_err16
        ret

disk_err16:
        push    cs
        pop     ds
        mov     ax, 0A000h
        mov     es, ax
        mov     di, 320
        mov     si, msg_diskerr
        call    print16
.halt:
        hlt
        jmp     .halt

;; ============================================================
;; fat12_next16 — FAT12次クラスタ取得
;; ============================================================
fat12_next16:
        push    bx
        push    dx
        push    es
        xor     bx, bx
        mov     es, bx
        mov     bx, ax
        shr     bx, 1
        add     bx, ax
        add     bx, FAT_BUF
        mov     dx, es:[bx]
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
        pop     es
        pop     dx
        pop     bx
        ret

;; ============================================================
;; print16 — 16ビットTVRAM表示
;; ============================================================
print16:
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
        jmp     print16
.done:
        ret

;; ============================================================
;; GDT
;; ============================================================
gdt:
        dq      0
        dw      0FFFFh
        dw      0
        db      0
        db      09Ah
        db      0CFh
        db      0
        dw      0FFFFh
        dw      0
        db      0
        db      092h
        db      0CFh
        db      0
gdt_end:

gdtr:
        dw      gdt_end - gdt - 1
gdtr_base:
        dd      0

;; ============================================================
;; データ
;; ============================================================
kern_name:      db 'VMKRNL  LZ4'
msg_loader:     db 'Loading vmkernel.lz4 from FAT12...', 0
msg_nokernel:   db 'VMKRNL.LZ4 not found!', 0
msg_pm:         db 'Entering PM...', 0
msg_diskerr:    db 'Disk Error!', 0
msg_ok32:       db 'Kernel loaded. Booting...', 0
msg_badmagic:   db 'Bad VK32 magic!', 0
msg_lz4err:     db 'LZ4 decode FAIL!', 0

var_cluster:    dw 0
var_size_lo:    dw 0
var_size_hi:    dw 0
var_load_seg:   dw 0
