bits 32

global asm_gfx_clear
global asm_gfx_draw_sprite
global asm_fill_plane_rect
global asm_copy_plane_rect

%define GFX_HEIGHT   400
%define GFX_BPL      80
%define GFX_PLANE_SZ 32000

section .text

; -------------------------------------------------------------------------
; void __cdecl asm_gfx_clear(unsigned char color, unsigned char *bb[4]);
; -------------------------------------------------------------------------
%define GFX_WIDTH 640
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
; void __cdecl asm_gfx_draw_sprite_core(int x, int y, int w, int h, int src_pitch, const u8 **planes, const u8 *mask, u8 **bb_array);
; -------------------------------------------------------------------------

global asm_gfx_draw_sprite_core

%define ARG_X     [ebp+8]
%define ARG_Y     [ebp+12]
%define ARG_W     [ebp+16]
%define ARG_H     [ebp+20]
%define ARG_SRCP  [ebp+24]
%define ARG_PLN   [ebp+28]
%define ARG_MASK  [ebp+32]
%define ARG_BB    [ebp+36]

%define L_BB0    [ebp-4]
%define L_BB1    [ebp-8]
%define L_BB2    [ebp-12]
%define L_BB3    [ebp-16]
%define L_SPR0   [ebp-20]
%define L_SPR1   [ebp-24]
%define L_SPR2   [ebp-28]
%define L_SPR3   [ebp-32]
%define L_IY     [ebp-36]
%define L_BX     [ebp-40]
%define L_DMASK  [ebp-44]

asm_gfx_draw_sprite_core:
    push ebp
    mov ebp, esp
    sub esp, 44
    push ebx
    push esi
    push edi

    ; Null check mask
    mov esi, ARG_MASK
    test esi, esi
    jz .end_func

    ; Cache BB pointers
    mov eax, ARG_BB
    mov edx, [eax]
    mov dword L_BB0, edx
    mov edx, [eax+4]
    mov dword L_BB1, edx
    mov edx, [eax+8]
    mov dword L_BB2, edx
    mov edx, [eax+12]
    mov dword L_BB3, edx

    ; Cache Plane pointers
    mov eax, ARG_PLN
    mov edx, [eax]
    mov dword L_SPR0, edx
    mov edx, [eax+4]
    mov dword L_SPR1, edx
    mov edx, [eax+8]
    mov dword L_SPR2, edx
    mov edx, [eax+12]
    mov dword L_SPR3, edx

    ; iy = 0
    mov dword L_IY, 0
.loop_y:
    ; 終了判定: iy < h
    mov eax, L_IY
    cmp eax, ARG_H
    jge .end_func

    ; dy = y + iy
    mov eax, ARG_Y
    add eax, L_IY
    
    ; if (dy < 0) continue
    cmp eax, 0
    jl .next_y
    ; if (dy >= GFX_HEIGHT) continue
    cmp eax, GFX_HEIGHT
    jge .next_y

    ; src_row = iy * src_pitch
    mov eax, L_IY
    imul eax, ARG_SRCP
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
    cmp eax, ARG_SRCP
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

    ; ebx = src_row + bx
    mov ebx, ecx
    add ebx, eax

    ; Load mask byte
    mov esi, ARG_MASK
    mov al, [esi + ebx]  ; al = mask byte

    ; ==== Xピクセルクリップ ====
    mov byte L_DMASK, 0xFF

    ; px_start = x + (bx * 8)
    mov ecx, L_BX
    shl ecx, 3
    add ecx, ARG_X       ; ecx = px_start

    ; -- 左クリップ --
    mov edx, 0
    sub edx, ecx
    cmp edx, 0
    jle .chk_right
    cmp edx, 8
    jge .skip_byte
    
    ; L_DMASK の上位 edx ビットを 0 にする
    push eax
    mov cl, dl
    mov al, L_DMASK
    shr al, cl
    mov L_DMASK, al
    pop eax
    ; ecx を px_start に戻す
    mov ecx, L_BX
    shl ecx, 3
    add ecx, ARG_X

.chk_right:
    ; -- 右クリップ --
    mov edx, ecx
    add edx, 8           ; edx = px_start + 8
    
    push eax
    mov eax, GFX_WIDTH  ; eax = VP_XMAX
    sub edx, eax
    pop eax
    
    cmp edx, 0
    jle .apply_mask
    cmp edx, 8
    jge .skip_byte
    
    ; L_DMASK の下位 edx ビットを 0 にする
    push eax
    mov cl, dl
    mov al, L_DMASK
    shl al, cl
    mov L_DMASK, al
    pop eax

.apply_mask:
    mov dh, L_DMASK      ; dh = draw_mask
    test dh, dh
    jz .skip_byte

    ; al (背景維持マスク) の、ビューポート外(dh=0)のビットを 1 にする
    mov dl, dh           ; dl = draw_mask
    not dl               ; dl = not draw_mask
    or al, dl            ; al のビューポート外ビットが 1 になる

    ; 描画用のレジスタを再構成
    mov edx, edi
    add edx, L_BX

    ; Plane 0
    mov esi, L_SPR0
    mov cl, [esi + ebx]
    mov ch, L_DMASK
    and cl, ch           ; ビューポート外の色データを消す
    mov esi, L_BB0
    mov ah, [esi + edx]
    and ah, al
    or  ah, cl
    mov [esi + edx], ah

    ; Plane 1
    mov esi, L_SPR1
    mov cl, [esi + ebx]
    mov ch, L_DMASK
    and cl, ch
    mov esi, L_BB1
    mov ah, [esi + edx]
    and ah, al
    or  ah, cl
    mov [esi + edx], ah

    ; Plane 2
    mov esi, L_SPR2
    mov cl, [esi + ebx]
    mov ch, L_DMASK
    and cl, ch
    mov esi, L_BB2
    mov ah, [esi + edx]
    and ah, al
    or  ah, cl
    mov [esi + edx], ah

    ; Plane 3
    mov esi, L_SPR3
    mov cl, [esi + ebx]
    mov ch, L_DMASK
    and cl, ch
    mov esi, L_BB3
    mov ah, [esi + edx]
    and ah, al
    or  ah, cl
    mov [esi + edx], ah

    ; Restore ecx (src_row)
    mov eax, L_IY
    imul eax, ARG_SRCP
    mov ecx, eax

.skip_byte:
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

; -------------------------------------------------------------------------
; void __cdecl asm_fill_plane_rect(u8 *start, int pitch, int rows,
;                                  int width_bytes, u8 fill_val);
;
; 1プレーンの矩形領域を fill_val で塗りつぶす。
; start を起点に width_bytes バイト分を fill し、pitch バイト進めて
; rows 行繰り返す。rep stosd + rep stosb による高速 fill。
; -------------------------------------------------------------------------
asm_fill_plane_rect:
    push ebp
    mov ebp, esp
    push edi
    push esi
    push ebx

    mov edi, [ebp+8]       ; start
    mov esi, [ebp+16]      ; rows
    mov ebx, [ebp+20]      ; width_bytes

    test esi, esi
    jle .fpr_done
    test ebx, ebx
    jle .fpr_done

    ; fill_val を 32bit に展開 (例: 0xFF → 0xFFFFFFFF)
    movzx eax, byte [ebp+24]
    mov ah, al
    mov ecx, eax
    shl ecx, 16
    or eax, ecx

    mov edx, [ebp+12]      ; pitch

.fpr_row:
    push edi

    mov ecx, ebx
    shr ecx, 2
    rep stosd

    mov ecx, ebx
    and ecx, 3
    rep stosb

    pop edi
    add edi, edx
    dec esi
    jnz .fpr_row

.fpr_done:
    pop ebx
    pop esi
    pop edi
    pop ebp
    ret

; -------------------------------------------------------------------------
; void __cdecl asm_copy_plane_rect(u8 *dst, int dst_pitch,
;                                  const u8 *src, int src_pitch,
;                                  int rows, int width_bytes);
;
; src から dst へ矩形領域をコピー。
; rep movsd + rep movsb による高速コピー。
; dst_pitch と src_pitch が異なる場合もサポート。
; -------------------------------------------------------------------------
asm_copy_plane_rect:
    push ebp
    mov ebp, esp
    sub esp, 4              ; ローカル変数: 行カウンタ
    push edi
    push esi
    push ebx

    mov edi, [ebp+8]       ; dst
    mov esi, [ebp+16]      ; src
    mov eax, [ebp+24]      ; rows
    mov ebx, [ebp+28]      ; width_bytes

    mov [ebp-4], eax        ; 行カウンタ保存

    test eax, eax
    jle .cpr_done
    test ebx, ebx
    jle .cpr_done

.cpr_row:
    push edi
    push esi

    mov ecx, ebx
    shr ecx, 2
    rep movsd

    mov ecx, ebx
    and ecx, 3
    rep movsb

    pop esi
    pop edi
    add edi, [ebp+12]      ; dst += dst_pitch
    add esi, [ebp+20]      ; src += src_pitch
    dec dword [ebp-4]
    jnz .cpr_row

.cpr_done:
    pop ebx
    pop esi
    pop edi
    mov esp, ebp
    pop ebp
    ret
