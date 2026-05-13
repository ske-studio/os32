;; ============================================================
;; loader_hdd_new.asm — OS32 HDD 新ローダー (ext2 + LZ4)
;;
;; boot_hdd.asm (IPL) により 0000:8000 にロードされ実行開始。
;; PM移行後、Cのboot_main()を呼び出してext2からvmkernel.lz4を
;; 読み込み→LZ4展開し、カーネルにジャンプする。
;;
;; 入力 (IPLからレジスタ渡し):
;;   AL = DA/UA, AH = ヘッド数, BL = セクタ/トラック
;;
;; メモリマップ:
;;   0x7F00-0x7F0F  パラメータ受け渡し (16bit→32bit)
;;   0x8000-0x9FFF  ローダー自身 (8KB)
;;   0x10000-       vmkernel.lz4 一時読み込み先
;;   0x100000       カーネル展開先
;;   0x200000       SQLite展開先
;;   0x9FFFC        PM スタック頂上
;; ============================================================

cpu 386

;; 16bit→32bit パラメータ受け渡し用の固定アドレス
;; (ELFシンボル参照を16bitコードで回避するため)
PARAM_AREA      EQU     7F00h
PARAM_DA_OFF    EQU     0       ;; 7F00h
PARAM_HEADS_OFF EQU     1       ;; 7F01h
PARAM_SPT_OFF   EQU     2       ;; 7F02h
PARAM_CYL_OFF   EQU     4       ;; 7F04h
PARAM_HEAD_OFF  EQU     6       ;; 7F06h
PARAM_SEC_OFF   EQU     7       ;; 7F07h
PARAM_BUF_SEG   EQU     8       ;; 7F08h
PARAM_BUF_OFF_16 EQU    10      ;; 7F0Ah
PARAM_RET_EIP   EQU     12      ;; 7F0Ch

section .text

;; ============================================================
;; 16bit エントリ (リアルモード)
;; -f elf32 のデフォルトは bits 32 なので明示的に bits 16 が必要
;; ============================================================
bits 16

global loader_entry
loader_entry:
        ;; IPLからのジオメトリ情報を固定アドレスに保存
        ;; (CS=0, DS未初期化なので cs: オーバーライド使用)
        mov     [cs:PARAM_AREA + PARAM_DA_OFF], al
        mov     [cs:PARAM_AREA + PARAM_HEADS_OFF], ah
        mov     [cs:PARAM_AREA + PARAM_SPT_OFF], bl

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

        ;; GDT ロード (インラインGDT — ELFシンボル参照を回避)
        ;; CS=0なのでGDTの線形アドレス = GDTのオフセットアドレス
        ;; gdtr16 は 0x8000 以降のどこかに配置されるが、
        ;; ここでは即値で計算して書き込む
        xor     eax, eax
        mov     ax, cs
        shl     eax, 4              ;; CS ベースアドレス (= 0)
        add     eax, dword gdt16    ;; GDTの線形アドレス
        mov     [dword gdtr16_base], eax
        lgdt    [dword gdtr16]

        ;; PM 遷移
        mov     eax, cr0
        or      al, 1
        mov     cr0, eax

        ;; 32bit PM へ far jmp (手動エンコード)
        db      066h
        db      0EAh
        dd      pm_entry
        dw      0008h

;; --- 16bitセクション内のインラインGDT ---
;; (bits 16 セクションに配置。リンカ配置アドレスは dword 参照で解決)
gdt16:
        dq      0
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
        ;; セレクタ 0x18: コード (base=0, limit=64KB, 16bit, Ring 0)
        dw      0FFFFh
        dw      0
        db      0
        db      09Ah
        db      0
        db      0
        ;; セレクタ 0x20: データ (base=0, limit=64KB, 16bit, Ring 0)
        dw      0FFFFh
        dw      0
        db      0
        db      092h
        db      0
        db      0
gdt16_end:
gdtr16:         dw      gdt16_end - gdt16 - 1
gdtr16_base:    dd      0

;; --- BIOS トランポリン (16bit PM -> RM -> BIOS -> PM) ---
pm16_trampoline:
        ;; 16bitデータセグメント (0x20)
        mov     ax, 20h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        
        ;; PEビットクリア
        mov     eax, cr0
        and     eax, ~1
        mov     cr0, eax
        
        ;; リアルモードへ far jmp
        jmp     0:rm_trampoline

rm_trampoline:
        ;; リアルモード (CS=0)
        xor     ax, ax
        mov     ds, ax
        mov     ss, ax
        sti
        
        ;; BIOSパラメータセット
        mov     al, [PARAM_AREA + PARAM_DA_OFF]
        mov     bx, 512
        mov     cx, [PARAM_AREA + PARAM_CYL_OFF]
        mov     dh, [PARAM_AREA + PARAM_HEAD_OFF]
        mov     dl, [PARAM_AREA + PARAM_SEC_OFF]
        mov     es, [PARAM_AREA + PARAM_BUF_SEG]
        mov     bp, [PARAM_AREA + PARAM_BUF_OFF_16]
        
        mov     ah, 06h
        int     1Bh
        
        cli
        
        ;; PEビットセット
        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax
        
        ;; 32bit PM へ far jmp
        db      066h
        db      0EAh
        dd      pm32_return_trampoline
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

        ;; 固定アドレスからパラメータを読み出してELFシンボルに保存
        mov     al, [PARAM_AREA + PARAM_DA_OFF]
        mov     [param_da], al
        mov     al, [PARAM_AREA + PARAM_HEADS_OFF]
        mov     [param_heads], al
        mov     al, [PARAM_AREA + PARAM_SPT_OFF]
        mov     [param_spt], al

        ;; 起動メッセージ
        mov     edi, 0A0000h
        mov     esi, msg_title
        call    pm_print

        ;; (IDE コントローラ初期化はBIOSに任せるため削除)

        ;; === BSS ゼロクリア (C言語の未初期化変数用) ===
        extern  __bss_start
        extern  __bss_end
        mov     edi, __bss_start
        mov     ecx, __bss_end
        sub     ecx, edi
        shr     ecx, 2           ;; バイト→DWORD
        xor     eax, eax
        rep     stosd

        ;; デバッグ: BSS OK
        mov     edi, 0A0000h + 320
        mov     esi, msg_dbg05
        call    pm_print

        ;; パラメータを再設定 (BSS ゼロクリアで上書きされた場合)
        mov     al, [PARAM_AREA + PARAM_DA_OFF]
        mov     [param_da], al
        mov     al, [PARAM_AREA + PARAM_HEADS_OFF]
        mov     [param_heads], al
        mov     al, [PARAM_AREA + PARAM_SPT_OFF]
        mov     [param_spt], al


        ;; デバッグ: Cメイン呼び出し前
        mov     edi, 0A0000h + 480
        mov     esi, msg_dbg1
        call    pm_print

        ;; === C メイン呼び出し ===
        extern  boot_main
        call    boot_main

        ;; デバッグ: boot_main戻り値をTVRAMに表示
        ;; EAXの値を16進1桁で表示 (0=成功, FD/FE/FF=エラー)
        push    eax
        mov     edi, 0A0000h + 640
        mov     esi, msg_dbg2
        call    pm_print
        ;; 戻り値の下位ニブルを16進文字で表示
        pop     eax
        push    eax
        and     al, 0Fh
        add     al, '0'
        cmp     al, '9'
        jbe     .hex_ok
        add     al, 7            ;; 'A'-'9'-1
.hex_ok:
        mov     ah, 0
        mov     [edi], ax
        push    edi
        add     edi, 2000h
        mov     byte [edi], 0E1h
        pop     edi

        pop     eax

        ;; エラー時 (EAX != 0) は停止
        or      eax, eax
        jnz     pm_halt

        ;; === メモリプロービング (1MB以上, 512KB刻み, 16MB上限) ===
        mov     esi, 00100000h
        mov     ecx, 1024        ;; 初期値 1024KB

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
        ;; kernel_main(u32 mem_kb, u32 boot_drive) — cdecl
        movzx   eax, byte [param_da]
        push    eax              ;; 第2引数: boot_drive
        push    ecx              ;; 第1引数: mem_kb
        push    dword 0          ;; ダミーリターンアドレス

        ;; カーネルにジャンプ
        mov     edi, 0A0000h + 800
        mov     esi, msg_booting
        call    pm_print

        db      0EAh
        dd      00100000h        ;; kentry (1MB)
        dw      0008h            ;; CS セレクタ


;; ============================================================
;; boot_read_sector_asm — cdecl ラッパー
;;   void boot_read_sector_asm(u32 lba, u8 *buf);
;; ============================================================

global boot_read_sector_asm
boot_read_sector_asm:
        push    ebp
        mov     ebp, esp
        push    edi
        mov     eax, [ebp+8]     ;; lba
        mov     edi, [ebp+12]    ;; buf
        call    pm_read_sector
        pop     edi
        pop     ebp
        ret


;; ============================================================
;; boot_print_asm — cdecl ラッパー
;;   void boot_print_asm(u32 tvram_addr, const char *msg);
;; ============================================================

global boot_print_asm
boot_print_asm:
        push    ebp
        mov     ebp, esp
        push    edi
        push    esi
        mov     edi, [ebp+8]     ;; tvram_addr
        mov     esi, [ebp+12]    ;; msg
        call    pm_print
        pop     esi
        pop     edi
        pop     ebp
        ret


pm32_return_trampoline:
        ;; 32bitデータセグメントに戻す
        mov     ax, 10h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        
        ;; 保存した EIP に戻る
        jmp     [PARAM_AREA + PARAM_RET_EIP]

;; ============================================================
;; pm_read_sector — BIOS トランポリンを使った1セクタ読み込み
;; 入力:  EAX = LBA, EDI = バッファアドレス (物理アドレス, 1MB未満)
;; 出力:  EDI += 512
;; 保存:  全レジスタ (EDI以外)
;; ============================================================

pm_read_sector:
        pushad
        
        ;; バッファアドレス変換 (EDI -> ES:BP)
        mov     ebx, edi
        shr     ebx, 4
        mov     [PARAM_AREA + PARAM_BUF_SEG], bx
        mov     ebx, edi
        and     ebx, 0Fh
        mov     [PARAM_AREA + PARAM_BUF_OFF_16], bx
        
        ;; LBA (EAX) -> CHS 変換
        xor     edx, edx
        movzx   ebx, byte [param_spt]
        div     ebx
        ;; EAX = Cyl*Head, DL = sector (0ベース)
        mov     [PARAM_AREA + PARAM_SEC_OFF], dl
        
        xor     edx, edx
        movzx   ebx, byte [param_heads]
        div     ebx
        ;; EAX = Cylinder, DL = head
        mov     [PARAM_AREA + PARAM_CYL_OFF], ax
        mov     [PARAM_AREA + PARAM_HEAD_OFF], dl
        
        ;; 戻り先アドレスを保存
        mov     dword [PARAM_AREA + PARAM_RET_EIP], .return_here
        
        ;; 16bit PMトランポリンへジャンプ
        jmp     0018h:pm16_trampoline
        
.return_here:
        popad
        add     edi, 512
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
;; パラメータ保存領域 (32bitコードから使用)
;; C コードから extern 参照される
;; ============================================================

global param_da
global param_heads
global param_spt

param_da:       db      0
param_heads:    db      0
param_spt:      db      0


;; ============================================================
;; メッセージ
;; ============================================================

msg_title:      db      'OS32 HDD Loader v3 (ext2+LZ4)', 0
msg_ide_err:    db      'IDE Timeout!', 0
msg_read_err:   db      'Read Error!', 0
msg_booting:    db      'Booting kernel...', 0
msg_dbg0:       db      '[0]IDE OK', 0
msg_dbg05:      db      '[0.5]BSS CLR', 0
msg_dbg1:       db      '[1]CALL MAIN', 0
msg_dbg2:       db      '[2]MAIN RET', 0
