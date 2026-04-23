; =========================================================================
; asm_tilemap.asm — タイルマップエンジン NASM 最適化ルーチン
; =========================================================================
; libtilemap の描画ホットパスを NASM で最適化。
;
; 1. asm_tile_pair_opaque  : 2タイルDWORD同時転送 (btf_fast用)
; 2. asm_tile_row_masked   : カバレッジマスク付き1行描画 (FtB用)
; 3. asm_byte_reverse_lut  : 256バイトLUT (H-flip用)
; =========================================================================

bits 32

global asm_tile_pair_opaque
global asm_tile_row_masked
global _asm_byte_reverse_lut

section .data

; =========================================================================
; 256バイト ビットリバースLUT (H-flip用)
; byte_reverse_lut[i] = ビット反転値 (例: 0b10110000 → 0b00001101)
; =========================================================================
_asm_byte_reverse_lut:
    ; 0x00-0x0F
    db 0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0
    db 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0
    ; 0x10-0x1F
    db 0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8
    db 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8
    ; 0x20-0x2F
    db 0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4
    db 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4
    ; 0x30-0x3F
    db 0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC
    db 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC
    ; 0x40-0x4F
    db 0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2
    db 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2
    ; 0x50-0x5F
    db 0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA
    db 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA
    ; 0x60-0x6F
    db 0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6
    db 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6
    ; 0x70-0x7F
    db 0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE
    db 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE
    ; 0x80-0x8F
    db 0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1
    db 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1
    ; 0x90-0x9F
    db 0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9
    db 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9
    ; 0xA0-0xAF
    db 0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5
    db 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5
    ; 0xB0-0xBF
    db 0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED
    db 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD
    ; 0xC0-0xCF
    db 0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3
    db 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3
    ; 0xD0-0xDF
    db 0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB
    db 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB
    ; 0xE0-0xEF
    db 0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7
    db 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7
    ; 0xF0-0xFF
    db 0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF
    db 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF


section .text

; =========================================================================
; void __cdecl asm_tile_pair_opaque(
;     u8 **bb_planes,   [ebp+8]   BB プレーンポインタ配列
;     int  dst_off,     [ebp+12]  初期バイトオフセット (dy * pitch + dx/8)
;     int  bb_pitch,    [ebp+16]  BBピッチ (80)
;     const u8 *tile_l, [ebp+20]  左タイル planes[0] の先頭
;     const u8 *tile_r, [ebp+24]  右タイル planes[0] の先頭
;     int  tile_psz);   [ebp+28]  TILE_PLANE_SZ (32)
;
; 2タイル (32px = 4bytes/row) を4プレーン × 16行 = 64 DWORD でBBに書き込む。
; タイルデータのレイアウト: planes[p] は先頭から p * tile_psz のオフセット。
; =========================================================================
asm_tile_pair_opaque:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov edi, [ebp+8]       ; bb_planes

    xor ecx, ecx           ; ecx = plane (0-3)
.plane_loop:
    cmp ecx, 4
    jge .done

    ; edi = bb_planes, edx = bb_planes[ecx] + dst_off
    mov edx, [edi + ecx*4]
    add edx, [ebp+12]      ; + dst_off

    ; esi_l = tile_l + ecx * tile_psz
    mov eax, ecx
    imul eax, [ebp+28]     ; * TILE_PLANE_SZ
    mov esi, [ebp+20]      ; tile_l
    add esi, eax           ; esi = tile_l + plane_offset

    ; ebx = tile_r + ecx * tile_psz
    mov ebx, [ebp+24]      ; tile_r
    add ebx, eax           ; ebx = tile_r + plane_offset

    ; 16行ループ (行インデックスは eax)
    push ecx               ; plane を退避
    xor ecx, ecx           ; row = 0
.row_loop:
    cmp ecx, 16
    jge .next_plane

    ; DWORD = (left[row*2] << 24) | (left[row*2+1] << 16)
    ;       | (right[row*2] << 8) | right[row*2+1]
    movzx eax, byte [esi]      ; left byte 0
    shl eax, 24
    movzx edi, byte [esi+1]    ; left byte 1
    shl edi, 16
    or eax, edi
    movzx edi, byte [ebx]      ; right byte 0
    shl edi, 8
    or eax, edi
    movzx edi, byte [ebx+1]    ; right byte 1
    or eax, edi

    mov [edx], eax              ; BB に書き込み

    ; 次の行: src += TILE_PITCH(2), dst += bb_pitch
    add esi, 2
    add ebx, 2
    add edx, [ebp+16]          ; + bb_pitch

    inc ecx
    jmp .row_loop

.next_plane:
    pop ecx                 ; plane 復帰
    mov edi, [ebp+8]        ; bb_planes 復帰
    inc ecx
    jmp .plane_loop

.done:
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret


; =========================================================================
; void __cdecl asm_tile_row_masked(
;     u8 **bb_planes,      [ebp+8]   BBプレーンポインタ配列
;     int  doff,           [ebp+12]  行オフセット (dy_row * pitch + dx/8)
;     const u8 *tile_row,  [ebp+16]  タイルデータ行先頭 (planes[0][row*2])
;     int  tile_psz,       [ebp+20]  TILE_PLANE_SZ (32) — 次プレーンへのストライド
;     u8  *cov_row);       [ebp+24]  カバレッジ行 (2バイト)
;
; カバレッジマスク付き1行 (16px = 2bytes × 4planes) のBB書き込み。
; cov_row[0..1] のビットが0の位置のみソースを書き込む。
; 処理後 cov_row を更新 (描画されたピクセルを追加マーク)。
; =========================================================================
asm_tile_row_masked:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov edi, [ebp+24]       ; cov_row
    movzx eax, byte [edi]   ; cov0
    movzx edx, byte [edi+1] ; cov1

    ; early-out: 全カバー済み
    cmp al, 0xFF
    jne .not_full
    cmp dl, 0xFF
    je .done_masked
.not_full:

    ; uncovered = ~cov
    mov cl, al
    not cl                  ; uncov0 = cl
    mov ch, dl
    not ch                  ; uncov1 = ch

    ; any0 = any1 = 0 (全プレーンのORを蓄積)
    xor ebx, ebx            ; bl = any0, bh = any1

    mov esi, [ebp+16]       ; tile_row (プレーン0の行先頭)
    mov edi, [ebp+8]        ; bb_planes

    ; === プレーン0 ===
    mov edx, [edi]           ; bb_planes[0]
    add edx, [ebp+12]       ; + doff

    movzx eax, byte [esi]    ; src0
    or bl, al                ; any0 |= src0
    and al, cl               ; src0 & uncov0
    mov ah, [edx]            ; dst0
    push ecx
    mov cl, [ebp-4]          ; cov0 を復帰... → 面倒なので別手法
    pop ecx
    ; dst = (dst & cov) | (src & uncov)
    ; → dst &= cov → dst |= (src & uncov)
    push eax
    mov al, [edx]
    push ecx
    mov cl, [ebp+24]         ; 直接参照は面倒
    pop ecx
    ; --- レジスタ不足のため手法変更: スタックに退避 ---
    pop eax

    ; ---- レジスタ配分を再設計 ----
    ; esi = tile_row
    ; edi = cov_row
    ; スタックローカルに bb_planes, doff, uncov を置く

    ; 簡潔な方式に切り替え: 4プレーンアンロールループ
    jmp .restart

.restart:
    ; スタックフレームを再構成
    mov edi, [ebp+24]       ; cov_row
    movzx eax, byte [edi]   ; cov0
    movzx edx, byte [edi+1] ; cov1

    ; uncov0 / uncov1 を計算してスタックに
    mov cl, al
    not cl                  ; uncov0
    mov ch, dl
    not ch                  ; uncov1
    ; [ebp-4] = uncov0, [ebp-8] = uncov1, [ebp-12]=cov0, [ebp-16]=cov1
    sub esp, 20
    mov [ebp-4], cl         ; uncov0
    mov [ebp-8], ch         ; uncov1
    mov [ebp-12], al        ; cov0
    mov [ebp-16], dl        ; cov1
    mov byte [ebp-20], 0    ; any0
    mov byte [ebp-24], 0    ; any1
    sub esp, 4              ; any0/any1 用に余分確保済み(上で20確保)

    mov esi, [ebp+16]       ; tile_row
    mov eax, [ebp+8]        ; bb_planes

    ; 4プレーン展開
    ; --- Plane 0 ---
    mov edi, [eax]           ; bb_planes[0]
    add edi, [ebp+12]       ; + doff

    movzx ecx, byte [esi]   ; src0_byte0
    or [ebp-20], cl          ; any0 |= src0
    and cl, [ebp-4]          ; src0 & uncov0
    mov dl, [edi]            ; dst byte0
    and dl, [ebp-12]         ; dst & cov0
    or dl, cl                ; | (src & uncov)
    mov [edi], dl

    movzx ecx, byte [esi+1] ; src0_byte1
    or [ebp-24], cl
    and cl, [ebp-8]
    mov dl, [edi+1]
    and dl, [ebp-16]
    or dl, cl
    mov [edi+1], dl

    ; --- Plane 1 ---
    mov ebx, [ebp+20]       ; tile_psz
    add esi, ebx            ; tile_row += stride → plane 1
    mov eax, [ebp+8]
    mov edi, [eax+4]         ; bb_planes[1]
    add edi, [ebp+12]

    movzx ecx, byte [esi]
    or [ebp-20], cl
    and cl, [ebp-4]
    mov dl, [edi]
    and dl, [ebp-12]
    or dl, cl
    mov [edi], dl

    movzx ecx, byte [esi+1]
    or [ebp-24], cl
    and cl, [ebp-8]
    mov dl, [edi+1]
    and dl, [ebp-16]
    or dl, cl
    mov [edi+1], dl

    ; --- Plane 2 ---
    add esi, ebx
    mov eax, [ebp+8]
    mov edi, [eax+8]
    add edi, [ebp+12]

    movzx ecx, byte [esi]
    or [ebp-20], cl
    and cl, [ebp-4]
    mov dl, [edi]
    and dl, [ebp-12]
    or dl, cl
    mov [edi], dl

    movzx ecx, byte [esi+1]
    or [ebp-24], cl
    and cl, [ebp-8]
    mov dl, [edi+1]
    and dl, [ebp-16]
    or dl, cl
    mov [edi+1], dl

    ; --- Plane 3 ---
    add esi, ebx
    mov eax, [ebp+8]
    mov edi, [eax+12]
    add edi, [ebp+12]

    movzx ecx, byte [esi]
    or [ebp-20], cl
    and cl, [ebp-4]
    mov dl, [edi]
    and dl, [ebp-12]
    or dl, cl
    mov [edi], dl

    movzx ecx, byte [esi+1]
    or [ebp-24], cl
    and cl, [ebp-8]
    mov dl, [edi+1]
    and dl, [ebp-16]
    or dl, cl
    mov [edi+1], dl

    ; --- カバレッジ更新 ---
    mov edi, [ebp+24]        ; cov_row
    mov al, [ebp-20]         ; any0
    and al, [ebp-4]          ; any0 & uncov0
    or [edi], al             ; cov_row[0] |= (any0 & uncov0)

    mov al, [ebp-24]         ; any1
    and al, [ebp-8]          ; any1 & uncov1
    or [edi+1], al           ; cov_row[1] |= (any1 & uncov1)

    add esp, 20              ; ローカル変数解放

.done_masked:
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
