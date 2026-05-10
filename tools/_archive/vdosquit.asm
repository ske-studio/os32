; vdosquit.asm
; DOS/V86 脱出用プログラム (COM形式)
; ポート0xFEにダミーデータを出力し、V86の#GPハンドラで終了させる
org 0x100
    mov al, 0x01
    out 0xFE, al
    mov ah, 0x4C
    int 0x21
