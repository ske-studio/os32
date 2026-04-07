;; ============================================================
;; loader_hdd.asm — OS32 HDD 第2ステージローダー (PM PIO版)
;;
;; boot_hdd.asm (IPL) により 0000:8000 にロードされ実行開始。
;;
;; 設計方針:
;;   - 16bitコードを最小限にし、可能な限り早くPMに遷移
;;   - PM移行後、IDE CHS PIOでカーネル全セクタをロード
;;   - PM_PIO_TEST.md の実証結果に基づく実装
;;
;; 入力 (IPLからレジスタ渡し):
;;   AL = DA/UA, AH = ヘッド数, BL = セクタ/トラック
;;
;; メモリマップ:
;;   0x8000-0x87FF  ローダー自身 (2048B)
;;   0x9000-        カーネルロード先
;;   0x9FFFC        PM スタック頂上
;;   0xA0000        TVRAM
;;
;; PM PIO 制約事項 (テスト実証済み):
;;   - LBAモードは NP21/W 非対応 → 常に CHS (0xA0)
;;   - CHSセクタ番号は1ベース → sector = LBA%SPT + 1
;;   - PM移行後の IDE SRST は必須
;; ============================================================

cpu 386

KERNEL_LBA  EQU     6           ;; カーネル開始LBA
LOAD_COUNT  EQU     400         ;; ロードセクタ数 (200KB)

section .text
        org 8000h

;; ============================================================
;; 16bit エントリ — 最小限のセットアップ後すぐにPM遷移
;; ============================================================

loader_entry:
        ;; IPLからのジオメトリ情報を保存
        ;; DS は不定のため CS プレフィックスを使用 (CS=0000)
        mov     [cs:param_da], al
        mov     [cs:param_heads], ah
        mov     [cs:param_spt], bl

        ;; セグメント初期化
        xor     ax, ax
        mov     ds, ax
        mov     ss, ax
        mov     sp, 7C00h

        ;; A20 ゲート有効化 (PC-98: ポート 0xF2)
        mov     al, 3
        out     0F2h, al

        ;; 割り込み・NMI 禁止
        cli
        xor     al, al
        out     50h, al

        ;; GDT ロード
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4
        add     eax, gdt
        mov     dword [gdtr_base], eax
        lgdt    [gdtr]

        ;; PM 遷移
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax

        ;; 32bit PM へ far jmp
        db      066h
        db      0EAh
        dd      pm_entry
        dw      0008h

;; ============================================================
;; 32bit PM コード
;; ============================================================
bits 32

pm_entry:
        ;; セグメントレジスタ初期化 (フラットモデル)
        mov     eax, 10h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 0009FFFCh

        ;; 起動メッセージ (行0)
        mov     edi, 0A0000h
        mov     esi, msg_title
        call    pm_print

        ;; === IDE コントローラ初期化 ===

        ;; バンクセレクト (IDE Bank 0)
        mov     dx, 0432h
        mov     al, 0
        out     dx, al

        ;; IDE ソフトリセット (SRST)
        ;; PM_PIO_TEST.md §4.2: BIOSの残留状態をクリアするため必須
        mov     dx, 074Ch
        mov     al, 06h             ;; SRST=1, nIEN=1
        out     dx, al

        mov     ecx, 10000h
srst_hold:
        dec     ecx
        jnz     srst_hold

        mov     dx, 074Ch
        mov     al, 02h             ;; SRST=0, nIEN=1
        out     dx, al

        ;; BSY クリア待ち (SRST後)
        mov     ecx, 200000h
srst_bsy:
        mov     dx, 64Eh
        in      al, dx
        test    al, 80h
        jz      srst_ok
        dec     ecx
        jnz     srst_bsy
        ;; タイムアウト
        mov     edi, 0A0000h + 160
        mov     esi, msg_ide_err
        call    pm_print
        jmp     pm_halt

srst_ok:
        ;; ロード開始メッセージ (行1)
        mov     edi, 0A0000h + 160
        mov     esi, msg_loading
        call    pm_print

        ;; === カーネルロード (CHS PIO) ===
        mov     edi, 00009000h       ;; カーネルロード先 (リニアアドレス)
        mov     eax, KERNEL_LBA      ;; 開始LBA
        mov     ecx, LOAD_COUNT      ;; セクタ数

load_kernel:
        call    pm_read_sector       ;; EAX保存, EDI += 512
        inc     eax
        dec     ecx
        jnz     load_kernel

        ;; ロード完了メッセージ (行2)
        mov     edi, 0A0000h + 320
        mov     esi, msg_booting
        call    pm_print

        ;; === メモリプロービング (1MB以上, 512KB刻み, 16MB上限) ===
        mov     esi, 00100000h       ;; 1MB
        mov     ecx, 1024            ;; 初期値 1024KB

probe_loop:
        mov     eax, [esi]
        mov     dword [esi], 0AA55AA55h
        cmp     dword [esi], 0AA55AA55h
        jne     probe_done
        mov     dword [esi], 055AA55AAh
        cmp     dword [esi], 055AA55AAh
        jne     probe_done
        mov     [esi], eax
        add     ecx, 512
        add     esi, 00080000h
        cmp     esi, 01000000h
        jb      probe_loop

probe_done:
        ;; === カーネルへジャンプ ===
        ;; kernel_main(uint32_t mem_kb, uint32_t boot_drive) — cdecl
        movzx   eax, byte [param_da]
        push    eax                  ;; 第2引数: boot_drive
        push    ecx                  ;; 第1引数: mem_kb
        push    dword 0              ;; ダミーリターンアドレス

        db      0EAh
        dd      00009000h
        dw      0008h


;; ============================================================
;; pm_read_sector — CHS PIO 1セクタ読み込み (32bit PM)
;;
;; 入力:  EAX = LBA, EDI = バッファアドレス
;; 出力:  EDI += 512
;; 保存:  EAX, EBX, ECX, EDX, EBP
;;
;; CHS変換:
;;   sector = (LBA % SPT) + 1      (1ベース)
;;   head   = (LBA / SPT) % heads
;;   cyl    = (LBA / SPT) / heads
;; ============================================================

pm_read_sector:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    ebp

        mov     ebp, eax             ;; LBA 退避

        ;; BSY クリア待ち
        call    pm_wait_bsy

        ;; LBA → CHS 変換
        mov     eax, ebp
        xor     edx, edx
        movzx   ebx, byte [param_spt]
        div     ebx                  ;; EAX = LBA/SPT, EDX = remainder
        inc     dl                   ;; DL = sector (1ベース)
        mov     cl, dl               ;; CL = sector

        xor     edx, edx
        movzx   ebx, byte [param_heads]
        div     ebx                  ;; EAX = cylinder, DL = head
        mov     ch, dl               ;; CH = head
        ;; EAX = cylinder

        ;; --- IDE レジスタ出力 ---

        ;; Sector Count (0x644) = 1
        push    eax
        mov     dx, 644h
        mov     al, 1
        out     dx, al

        ;; Sector Number (0x646)
        mov     dx, 646h
        mov     al, cl
        out     dx, al

        ;; Cylinder Low (0x648)
        pop     eax
        push    eax
        mov     dx, 648h
        out     dx, al

        ;; Cylinder High (0x64A)
        mov     dx, 64Ah
        mov     al, ah
        out     dx, al

        ;; Drive/Head (0x64C) — CHS mode = 0xA0
        mov     dx, 64Ch
        mov     al, ch
        and     al, 0Fh
        or      al, 0A0h
        out     dx, al
        pop     eax                  ;; スタッククリーン

        ;; 400ns ウェイト (Alternate Status 空読み)
        mov     dx, 64Eh
        in      al, dx
        in      al, dx
        in      al, dx
        in      al, dx

        ;; READ SECTORS コマンド (0x20)
        mov     al, 20h
        out     dx, al

        ;; DRQ 待ち
        call    pm_wait_drq

        ;; データ読み込み (256 words = 512 bytes)
        mov     ecx, 256
        mov     dx, 640h
pio_read:
        in      ax, dx
        mov     [edi], ax
        add     edi, 2
        dec     ecx
        jnz     pio_read

        ;; IRQ クリア (ステータス空読み) + BSY 待ち
        mov     dx, 64Eh
        in      al, dx
        call    pm_wait_bsy

        pop     ebp
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret


;; ============================================================
;; pm_wait_bsy — BSY=0 待ち (タイムアウト → エラー停止)
;; ============================================================

pm_wait_bsy:
        push    ecx
        push    edx
        mov     ecx, 100000h
wbsy_loop:
        mov     dx, 64Eh
        in      al, dx
        test    al, 80h
        jz      wbsy_ok
        dec     ecx
        jnz     wbsy_loop
        mov     edi, 0A0000h + 160
        mov     esi, msg_ide_err
        call    pm_print
        jmp     pm_halt
wbsy_ok:
        pop     edx
        pop     ecx
        ret


;; ============================================================
;; pm_wait_drq — DRQ=1 待ち (エラーチェック付き)
;; ============================================================

pm_wait_drq:
        push    ecx
        push    edx
        mov     ecx, 100000h
wdrq_loop:
        mov     dx, 64Eh
        in      al, dx
        test    al, 80h             ;; BSY?
        jnz     wdrq_cont
        test    al, 21h             ;; ERR | DF?
        jnz     wdrq_err
        test    al, 08h             ;; DRQ?
        jnz     wdrq_ok
wdrq_cont:
        dec     ecx
        jnz     wdrq_loop
        mov     edi, 0A0000h + 160
        mov     esi, msg_ide_err
        call    pm_print
        jmp     pm_halt
wdrq_err:
        mov     edi, 0A0000h + 160
        mov     esi, msg_read_err
        call    pm_print
        jmp     pm_halt
wdrq_ok:
        pop     edx
        pop     ecx
        ret


;; ============================================================
;; pm_print — TVRAM 文字列表示 (32bit PM)
;; ============================================================

pm_print:
        push    eax
ppr_loop:
        mov     al, [esi]
        inc     esi
        or      al, al
        jz      ppr_done
        mov     ah, 0
        mov     [edi], ax
        push    edi
        add     edi, 2000h
        mov     byte [edi], 0E1h
        pop     edi
        add     edi, 2
        jmp     ppr_loop
ppr_done:
        pop     eax
        ret


;; ============================================================
;; pm_halt — 停止
;; ============================================================

pm_halt:
        hlt
        jmp     pm_halt


;; ============================================================
;; GDT
;; ============================================================

gdt:
        dq      0                    ;; ヌルディスクリプタ

        ;; セレクタ 0x08: コード (base=0, limit=4GB, 32bit, Ring 0)
        dw      0FFFFh
        dw      0
        db      0
        db      09Ah
        db      0CFh
        db      0

        ;; セレクタ 0x10: データ (base=0, limit=4GB, 32bit, Ring 0)
        dw      0FFFFh
        dw      0
        db      0
        db      092h
        db      0CFh
        db      0
gdt_end:

gdtr:           dw      gdt_end - gdt - 1
gdtr_base:      dd      0


;; ============================================================
;; パラメータ保存領域 (IPLからレジスタ渡しで受け取った値)
;; ============================================================

param_da:       db      0
param_heads:    db      0
param_spt:      db      0


;; ============================================================
;; メッセージ
;; ============================================================

msg_title:      db      'OS32 HDD Loader v2', 0
msg_loading:    db      'Loading kernel (PIO)...', 0
msg_booting:    db      'Kernel loaded. Booting...', 0
msg_ide_err:    db      'IDE Timeout!', 0
msg_read_err:   db      'Read Error!', 0

        times   2048 - ($ - $$) db 0
