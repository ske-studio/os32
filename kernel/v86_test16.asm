;; V86 ゲストのスモークテスト (Phase 3)
;;   - IVT[08h] に自前 ISR を仕込む
;;   - STI して IRQ0 の反射を待つ
;;   - ISR が呼ばれるたびにカウンタを増やす
;;   - 3 回来たら継続の証拠を書いて HLT
;;
;; 配置: CS = 0x8A00 (linear 0x8A000), offset 0
;; データ: 0x8C00:0000 = magic1, :0002 = magic2, :0004 = IRQ カウンタ

cpu 386
bits 16
org 0

CODE_SEG    equ 0x8A00
DATA_SEG    equ 0x8C00

start:
        ;; --- (1) メモリ書き込み: magic1 = 0x1234 ---
        mov     ax, DATA_SEG
        mov     es, ax
        xor     di, di
        mov     ax, 0x1234
        mov     [es:di], ax

        ;; --- (2) 許可ポートへの OUT / IN (トラップしてはいけない) ---
        mov     al, 0
        out     0x5F, al
        in      al, 0x60

        ;; --- (3) 拒否ポート (マスタ PIC) — #GP で捕まるはず ---
        in      al, 0x00
        mov     al, 0x55
        out     0x00, al

        ;; --- (4) IVT[08h] に自前 ISR を登録 ---
        xor     ax, ax
        mov     es, ax
        mov     word [es:0x20], isr8
        mov     word [es:0x22], CODE_SEG

        ;; --- (5) カウンタ初期化 ---
        mov     ax, DATA_SEG
        mov     ds, ax
        mov     word [4], 0

        ;; --- (6) 割り込みを開けて IRQ 反射を待つ ---
        sti
.wait:
        mov     ax, [4]
        cmp     ax, 3
        jb      .wait
        cli

        ;; --- (7) 継続の証拠: magic2 = 0x5678 ---
        mov     word [2], 0x5678

        hlt

;; ------------------------------------------------------------
;; IRQ0 反射先。ゲスト側の割り込みハンドラ。
;; ------------------------------------------------------------
isr8:
        push    ax
        push    ds
        mov     ax, DATA_SEG
        mov     ds, ax
        inc     word [4]
        pop     ds
        pop     ax
        iret
