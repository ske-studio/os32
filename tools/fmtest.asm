; ========================================================================
; fmtest.asm — PC-98 SSG Sound Test (IPL Boot, 256バイト以内)
;
; SSG 3和音(C-E-G)を鳴らして画面に表示する最小テスト
; ========================================================================

[BITS 16]
[ORG 0]

OPN_ADDR equ 0x0188
OPN_DATA equ 0x018A

start:
    cli
    mov ax, cs
    mov ds, ax
    mov ss, ax
    mov sp, 0x0100
    sti

    ; === "SSG" 表示 (TVRAM 0xA000) ===
    push es
    mov ax, 0xA000
    mov es, ax
    xor di, di
    mov word [es:di+0],  0x0053   ; 'S'
    mov word [es:di+2],  0x0053   ; 'S'
    mov word [es:di+4],  0x0047   ; 'G'
    mov ax, 0xA200
    mov es, ax
    xor di, di
    mov ax, 0x00E1
    stosw
    stosw
    stosw
    pop es

    ; === SSG初期化 ===
    xor cx, cx
.clr:
    mov al, cl
    xor ah, ah
    call opn_wr
    inc cl
    cmp cl, 14
    jb .clr

    ; Mixer OFF
    mov al, 7
    mov ah, 0x3F
    call opn_wr

    ; === Ch.A = C4 (period 0x77) ===
    mov al, 0
    mov ah, 0x77
    call opn_wr
    mov al, 1
    xor ah, ah
    call opn_wr
    mov al, 8
    mov ah, 0x0F
    call opn_wr
    ; Mixer: A on
    mov al, 7
    mov ah, 0x3E
    call opn_wr

    call delay_1s

    ; === Ch.B = E4 (period 0x5F) ===
    mov al, 2
    mov ah, 0x5F
    call opn_wr
    mov al, 3
    xor ah, ah
    call opn_wr
    mov al, 9
    mov ah, 0x0F
    call opn_wr
    ; Mixer: A+B on
    mov al, 7
    mov ah, 0x3C
    call opn_wr

    call delay_1s

    ; === Ch.C = G4 (period 0x50) ===
    mov al, 4
    mov ah, 0x50
    call opn_wr
    mov al, 5
    xor ah, ah
    call opn_wr
    mov al, 10
    mov ah, 0x0F
    call opn_wr
    ; Mixer: A+B+C on
    mov al, 7
    mov ah, 0x38
    call opn_wr

    ; "OK" 表示
    push es
    mov ax, 0xA000
    mov es, ax
    mov di, 8
    mov word [es:di], 0x004F      ; 'O'
    mov word [es:di+2], 0x004B    ; 'K'
    mov ax, 0xA200
    mov es, ax
    mov di, 8
    mov ax, 0x00E1
    stosw
    stosw
    pop es

.halt:
    hlt
    jmp .halt

; --- opn_wr: AL=reg, AH=data ---
opn_wr:
    push bx
    push dx
    mov bl, ah
    mov dx, OPN_ADDR
.bsy:
    push ax
    in al, dx
    test al, 0x80
    pop ax
    jnz .bsy
    out dx, al
    push cx
    mov cx, 8
.w1: loop .w1
    pop cx
    mov dx, OPN_DATA
    mov al, bl
    out dx, al
    push cx
    mov cx, 20
.w2: loop .w2
    pop cx
    pop dx
    pop bx
    ret

; --- delay_1s ---
delay_1s:
    push cx
    push dx
    mov dx, 30
.o: xor cx, cx
.i: loop .i
    dec dx
    jnz .o
    pop dx
    pop cx
    ret
