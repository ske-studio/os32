bits 32
section .text

global kmemcpy
global memcpy
global kmemset
global memset

; ========================================================================
; void *kmemcpy(void *dst, const void *src, u32 n)
; void *memcpy(void *dst, const void *src, u32 n)
; ========================================================================
kmemcpy:
memcpy:
    push ebp
    mov ebp, esp
    push edi
    push esi
    
    mov edi, [ebp+8]    ; dst
    mov esi, [ebp+12]   ; src
    mov ecx, [ebp+16]   ; n
    
    mov eax, edi        ; 戻り値 eax = dst

    cmp ecx, 4
    jl .do_bytes

    ; dst アライメント (4バイト境界になるまで1バイトずつ)
    test edi, 3
    jz .aligned
.align_loop:
    movsb
    dec ecx
    test edi, 3
    jnz .align_loop

.aligned:
    ; DWORD単位の転送
    mov edx, ecx
    shr ecx, 2
    jz .do_bytes_recover
    rep movsd

.do_bytes_recover:
    mov ecx, edx
    and ecx, 3
    jz .done

.do_bytes:
    rep movsb

.done:
    pop esi
    pop edi
    pop ebp
    ret

; ========================================================================
; void *kmemset(void *dst, int val, u32 n)
; void *memset(void *dst, int val, u32 n)
; ========================================================================
kmemset:
memset:
    push ebp
    mov ebp, esp
    push edi
    
    mov edi, [ebp+8]    ; dst
    mov eax, [ebp+12]   ; val
    mov ecx, [ebp+16]   ; n
    
    mov edx, edi        ; 戻り値のために保存

    ; valueを4バイト(DWORD)に拡張 (imul は 80386以降で有効)
    and eax, 0xFF
    imul eax, eax, 0x01010101
    
    cmp ecx, 4
    jl .do_bytes

    ; dst アライメント
    test edi, 3
    jz .aligned
.align_loop:
    stosb
    dec ecx
    test edi, 3
    jnz .align_loop

.aligned:
    ; DWORD単位の書き込み
    push ecx
    shr ecx, 2
    jz .do_bytes_recover
    rep stosd
    pop ecx

.do_bytes_recover:
    and ecx, 3
    jz .done

.do_bytes:
    rep stosb

.done:
    mov eax, edx        ; 戻り値 eax = dst
    pop edi
    pop ebp
    ret
