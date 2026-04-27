;; ============================================================
;; kentry.asm — カーネルエントリポイント
;; kernel.binの先頭に配置され、BSSクリア後にkernel_mainにジャンプする
;; ============================================================

cpu 386
bits 32

extern kernel_main
extern __bss_start
extern __bss_end
extern __sqlite_data_end
extern __sqlite_end

section .text

global kentry
kentry:
        ;; ============================================================
        ;; FPU (x87) 初期化
        ;; CR0.EM (bit2) = 0: FPU命令をソフトウェアエミュレーションしない
        ;; CR0.MP (bit1) = 1: WAIT/FWAIT でタスクスイッチ確認
        ;; CR0.NE (bit5) = 1: x87例外を内部(INT16)で処理 (外部IRQ13不使用)
        ;; ============================================================
        mov     eax, cr0
        and     eax, ~(1 << 2)    ;; CR0.EM = 0 (FPU命令を直接実行)
        or      eax, (1 << 1)     ;; CR0.MP = 1
        or      eax, (1 << 5)     ;; CR0.NE = 1
        mov     cr0, eax
        fninit                     ;; FPU を既知の状態にリセット
        ;; BSS領域ゼロクリア (ベアメタルではCRT0がないため手動で行う)
        mov     edi, __bss_start
        mov     ecx, __bss_end
        sub     ecx, edi
        shr     ecx, 2            ;; DWORD数に変換
        xor     eax, eax
        rep     stosd

        ;; SQLite BSS領域ゼロクリア (ブートローダーが.sqlite_bssをロードしないため)
        mov     edi, __sqlite_data_end
        mov     ecx, __sqlite_end
        sub     ecx, edi
        shr     ecx, 2
        xor     eax, eax
        rep     stosd

        jmp     kernel_main
