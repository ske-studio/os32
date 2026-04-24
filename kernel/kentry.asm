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
