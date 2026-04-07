bits 32

global asm_gfx_clear
global asm_gfx_draw_sprite

%define GFX_HEIGHT   400
%define GFX_BPL      80
%define GFX_PLANE_SZ 32000

section .text

; -------------------------------------------------------------------------
; void __cdecl asm_gfx_clear(unsigned char color, unsigned char *bb[4]);
; -------------------------------------------------------------------------
asm_gfx_clear:
    push ebp
    mov ebp, esp
    push edi
    push esi

    mov dl, [ebp+8]      ; color
    mov esi, [ebp+12]    ; bb array pointer

    ; プレーン 0 (B)
    mov edi, [esi]       ; bb[0]
    mov ecx, GFX_PLANE_SZ / 4
    xor eax, eax
    test dl, 1
    jz .p0_fill
    not eax
.p0_fill:
    rep stosd

    ; プレーン 1 (R)
    mov edi, [esi+4]     ; bb[1]
    mov ecx, GFX_PLANE_SZ / 4
    xor eax, eax
    test dl, 2
    jz .p1_fill
    not eax
.p1_fill:
    rep stosd

    ; プレーン 2 (G)
    mov edi, [esi+8]     ; bb[2]
    mov ecx, GFX_PLANE_SZ / 4
    xor eax, eax
    test dl, 4
    jz .p2_fill
    not eax
.p2_fill:
    rep stosd

    ; プレーン 3 (I)
    mov edi, [esi+12]    ; bb[3]
    mov ecx, GFX_PLANE_SZ / 4
    xor eax, eax
    test dl, 8
    jz .p3_fill
    not eax
.p3_fill:
    rep stosd

    pop esi
    pop edi
    pop ebp
    ret

; -------------------------------------------------------------------------
; void __cdecl asm_gfx_draw_sprite(int x, int y, const GFX_Sprite *spr, unsigned char *bb[4]);
; -------------------------------------------------------------------------
; GFX_Sprite struct (36 bytes):
;   0: w (int)
;   4: h (int)
;   8: pitch (int)
;  12: planes[0] (ptr)
;  16: planes[1] (ptr)
;  20: planes[2] (ptr)
;  24: planes[3] (ptr)
;  28: mask (ptr)
;  32: _pool_idx (int)

%define ARG_X    [ebp+8]
%define ARG_Y    [ebp+12]
%define ARG_SPR  [ebp+16]
%define ARG_BB   [ebp+20]

%define L_BB0    [ebp-4]
%define L_BB1    [ebp-8]
%define L_BB2    [ebp-12]
%define L_BB3    [ebp-16]
%define L_SPR0   [ebp-20]
%define L_SPR1   [ebp-24]
%define L_SPR2   [ebp-28]
%define L_SPR3   [ebp-32]
%define L_MASK   [ebp-36]
%define L_SRC_P  [ebp-40]  ; src_pitch
%define L_IY     [ebp-44]  ; iy loop counter
%define L_BX     [ebp-48]  ; bx loop counter

asm_gfx_draw_sprite:
    push ebp
    mov ebp, esp
    sub esp, 48
    push ebx
    push esi
    push edi

    ; Null check spr
    mov esi, ARG_SPR
    test esi, esi
    jz .end_func

    ; Cache pointers
    mov eax, ARG_BB
    mov edx, [eax]
    mov dword L_BB0, edx
    mov edx, [eax+4]
    mov dword L_BB1, edx
    mov edx, [eax+8]
    mov dword L_BB2, edx
    mov edx, [eax+12]
    mov dword L_BB3, edx

    mov edx, [esi+12]
    mov dword L_SPR0, edx
    mov edx, [esi+16]
    mov dword L_SPR1, edx
    mov edx, [esi+20]
    mov dword L_SPR2, edx
    mov edx, [esi+24]
    mov dword L_SPR3, edx
    mov edx, [esi+28]
    mov dword L_MASK, edx

    mov edx, [esi+8]
    mov dword L_SRC_P, edx

    ; iy = 0
    mov dword L_IY, 0
.loop_y:
    ; 終了判定: iy < spr->h
    mov esi, ARG_SPR
    mov eax, L_IY
    cmp eax, [esi+4]
    jge .end_func

    ; dy = y + iy
    mov eax, ARG_Y
    add eax, L_IY
    
    ; if (dy < 0 || dy >= GFX_HEIGHT) continue
    cmp eax, 0
    jl .next_y
    cmp eax, GFX_HEIGHT
    jge .next_y

    ; src_row = iy * src_pitch
    mov eax, L_IY
    imul eax, L_SRC_P
    mov ecx, eax  ; ecx = src_row

    ; dst_row = dy * GFX_BPL + (x >> 3)
    mov eax, ARG_Y
    add eax, L_IY
    imul eax, GFX_BPL
    mov edx, ARG_X
    sar edx, 3
    add eax, edx
    mov edi, eax  ; edi = dst_row (byte offset into BB)

    ; bx = 0
    mov dword L_BX, 0
.loop_x:
    mov eax, L_BX
    cmp eax, L_SRC_P
    jge .next_y

    ; dx_byte = (x >> 3) + bx
    mov edx, ARG_X
    sar edx, 3
    add edx, eax
    ; if (dx_byte < 0 || dx_byte >= GFX_BPL) continue
    cmp edx, 0
    jl .next_x
    cmp edx, GFX_BPL
    jge .next_x

    ; eax = bx
    mov ebx, ecx  ; src_row
    add ebx, eax  ; ebx = src_row + bx

    mov edx, edi  ; dst_row
    add edx, eax  ; edx = dst_row + bx

    ; Load mask byte
    mov esi, L_MASK
    mov al, [esi + ebx]  ; al = mask byte

    ; Plane 0
    mov esi, L_SPR0
    mov cl, [esi + ebx]
    mov esi, L_BB0
    mov ah, [esi + edx]
    and ah, al
    or  ah, cl
    mov [esi + edx], ah

    ; Plane 1
    mov esi, L_SPR1
    mov cl, [esi + ebx]
    mov esi, L_BB1
    mov ah, [esi + edx]
    and ah, al
    or  ah, cl
    mov [esi + edx], ah

    ; Plane 2
    mov esi, L_SPR2
    mov cl, [esi + ebx]
    mov esi, L_BB2
    mov ah, [esi + edx]
    and ah, al
    or  ah, cl
    mov [esi + edx], ah

    ; Plane 3
    mov esi, L_SPR3
    mov cl, [esi + ebx]
    mov esi, L_BB3
    mov ah, [esi + edx]
    and ah, al
    or  ah, cl
    mov [esi + edx], ah

    ; Restore ecx (src_row)
    mov eax, L_IY
    imul eax, L_SRC_P
    mov ecx, eax

.next_x:
    inc dword L_BX
    jmp .loop_x

.next_y:
    inc dword L_IY
    jmp .loop_y

.end_func:
    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret

; -------------------------------------------------------------------------
; void __cdecl asm_kcg_draw_font(int x, int y, const unsigned char *pat, int w_bytes, int h_lines, unsigned char fg, unsigned char *bb[4]);
; -------------------------------------------------------------------------
%define ARG_X    [ebp+8]
%define ARG_Y    [ebp+12]
%define ARG_PAT  [ebp+16]
%define ARG_WB   [ebp+20]
%define ARG_HL   [ebp+24]
%define ARG_FG   [ebp+28]
%define ARG_BB   [ebp+32]

%define L_BB0    [ebp-4]
%define L_BB1    [ebp-8]
%define L_BB2    [ebp-12]
%define L_BB3    [ebp-16]
%define L_IY     [ebp-20]
%define L_BX     [ebp-24]

global asm_kcg_draw_font

asm_kcg_draw_font:
    push ebp
    mov ebp, esp
    sub esp, 24
    push ebx
    push esi
    push edi

    ; Null check pat
    mov esi, ARG_PAT
    test esi, esi
    jz .kcg_end

    ; Cache pointers
    mov eax, ARG_BB
    mov edx, [eax]
    mov dword L_BB0, edx
    mov edx, [eax+4]
    mov dword L_BB1, edx
    mov edx, [eax+8]
    mov dword L_BB2, edx
    mov edx, [eax+12]
    mov dword L_BB3, edx

    ; iy = 0
    mov dword L_IY, 0
.kcg_loop_y:
    mov esi, ARG_PAT
    mov eax, L_IY
    cmp eax, ARG_HL
    jge .kcg_end

    ; dy = y + iy
    mov eax, ARG_Y
    add eax, L_IY
    
    ; if (dy < 0 || dy >= GFX_HEIGHT) continue
    cmp eax, 0
    jl .kcg_next_y
    cmp eax, GFX_HEIGHT
    jge .kcg_next_y

    ; src_row = iy * w_bytes
    mov eax, L_IY
    imul eax, ARG_WB
    mov ecx, eax  ; ecx = src_row

    ; dst_row = dy * GFX_BPL + (x >> 3)
    mov eax, ARG_Y
    add eax, L_IY
    imul eax, GFX_BPL
    mov edx, ARG_X
    sar edx, 3
    add eax, edx
    mov edi, eax  ; edi = dst_row (byte offset into BB)

    ; bx = 0
    mov dword L_BX, 0
.kcg_loop_x:
    mov eax, L_BX
    cmp eax, ARG_WB
    jge .kcg_next_y

    ; dx_byte = (x >> 3) + bx
    mov edx, ARG_X
    sar edx, 3
    add edx, eax
    ; if (dx_byte < 0 || dx_byte >= GFX_BPL) continue
    cmp edx, 0
    jl .kcg_next_x
    cmp edx, GFX_BPL
    jge .kcg_next_x

    ; ebx = src pointer offset
    mov ebx, ecx  ; src_row
    add ebx, eax  ; src_row + bx

    ; Load pat byte
    mov esi, ARG_PAT
    mov al, [esi + ebx]  ; al = pat byte
    test al, al
    jz .kcg_next_x       ; if pat byte is 0, nothing to draw

    ; edx = dst pointer offset
    mov edx, edi  ; dst_row
    mov ebx, L_BX
    add edx, ebx  ; dst_row + bx
    
    mov bl, ARG_FG   ; bl = fg color

    ; --- Plane 0 ---
    mov esi, L_BB0
    mov ah, [esi + edx]
    test bl, 1
    jz .k_clr0
    or ah, al
    jmp .k_wr0
.k_clr0:
    mov bh, al
    not bh
    and ah, bh
.k_wr0:
    mov [esi + edx], ah

    ; --- Plane 1 ---
    mov esi, L_BB1
    mov ah, [esi + edx]
    test bl, 2
    jz .k_clr1
    or ah, al
    jmp .k_wr1
.k_clr1:
    mov bh, al
    not bh
    and ah, bh
.k_wr1:
    mov [esi + edx], ah

    ; --- Plane 2 ---
    mov esi, L_BB2
    mov ah, [esi + edx]
    test bl, 4
    jz .k_clr2
    or ah, al
    jmp .k_wr2
.k_clr2:
    mov bh, al
    not bh
    and ah, bh
.k_wr2:
    mov [esi + edx], ah

    ; --- Plane 3 ---
    mov esi, L_BB3
    mov ah, [esi + edx]
    test bl, 8
    jz .k_clr3
    or ah, al
    jmp .k_wr3
.k_clr3:
    mov bh, al
    not bh
    and ah, bh
.k_wr3:
    mov [esi + edx], ah

.kcg_next_x:
    inc dword L_BX
    jmp .kcg_loop_x

.kcg_next_y:
    inc dword L_IY
    jmp .kcg_loop_y

.kcg_end:
    pop edi
    pop esi
    pop ebx
    mov esp, ebp
    pop ebp
    ret
