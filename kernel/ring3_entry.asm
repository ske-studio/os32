;; ============================================================
;; ring3_entry.asm — リング3 (CPL=3) システムコール入口 (v2 M1/M2)
;;
;; CPL=3 プログラムが KAPI トランポリンのスタブ (mov eax,slot; int 0x80; ret)
;; を呼ぶとここに入る。IDT ゲートは DPL=3 (kernel/idt.c)。CPU は特権遷移で
;; TSS.SS0:ESP0 のカーネルスタックに切り替え SS/ESP/EFLAGS/CS/EIP を積む
;; (エラーコードなし)。
;;
;; int80_stub: pushad でフレームを作り、C の ring3_syscall_dispatch(frame*) へ。
;;   フレーム: [0..7]=pushad(EDI..EAX), [8]=EIP [9]=CS [10]=EFLAGS
;;             [11]=userESP [12]=userSS。eax(=[7]) が slot。
;; ディスパッチャが本物の wrap を呼び戻り値を frame[7](=eax) に書く。
;; sys_exit / 範囲外 slot は longjmp するのでここへ戻らない。それ以外は
;; popad で eax=戻り値を復元し、CPL=3 へ iretd で戻る。
;;
;; 【重要】CPL=3 への iretd 復帰時はユーザデータセグメント (USER_DS) を
;; 復元する (isr_stub.asm の IRETD_USER と同じ理由: RESTORE_KSEG で
;; DS/ES/FS/GS が KERNEL_DS になっており、iretd はこれを戻さないため)。
;; ============================================================

cpu 386

extern ring3_syscall_dispatch

;; ユーザデータセグメント (CONTRACTS C1, kernel/gdt.h の USER_DS)
USER_DS     equ 0x2B
KERNEL_DS   equ 0x10

section .text

global int80_stub
int80_stub:
        ;; ゲートは割込みゲートなので IF は既にクリア済み。
        pushad                          ;; フレーム先頭 = esp

        ;; C ハンドラはカーネルデータセグメント前提。CPL=3 由来では
        ;; DS/ES/FS/GS が USER_DS なので復元する (RESTORE_KSEG 相当)。
        mov     ax, KERNEL_DS
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax

        mov     eax, esp                ;; frame ptr (pushad 先頭)
        push    eax
        call    ring3_syscall_dispatch  ;; sys_exit/範囲外は戻らない (longjmp)
        add     esp, 4

        popad                           ;; eax = 戻り値 (dispatcher が frame[7] に格納)

        ;; --- IRETD_USER: CPL=3 へ戻るならユーザセグメントを復元 ---
        ;; popad 後 esp -> [EIP][CS][EFLAGS][userESP][userSS]
        test    dword [esp + 8], 0x00020000   ;; EFLAGS.VM? (トランポリンは常に非V86)
        jnz     .do_iret
        test    dword [esp + 4], 3            ;; CS.RPL == 3 (CPL=3 へ戻る)?
        jz      .do_iret
        push    eax
        mov     ax, USER_DS
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        pop     eax
.do_iret:
        iretd

;; ============================================================
;; u32 __cdecl kapi_invoke(void *wrapfn, const void *args_src, u32 nbytes)
;;
;; args_src から nbytes バイトをカレント (カーネル) スタックへコピーし、
;; wrapfn を cdecl 呼び出しして戻り値 (eax) を返す。nbytes は 4 の倍数。
;; cdecl なので呼び出し側 (この関数) がコピーした引数を掃除する。
;; ============================================================
global kapi_invoke
kapi_invoke:
        push    ebp
        mov     ebp, esp
        push    esi
        push    edi
        push    ebx
        mov     ebx, [ebp + 8]          ;; wrapfn
        mov     esi, [ebp + 12]         ;; args_src
        mov     ecx, [ebp + 16]         ;; nbytes
        sub     esp, ecx                ;; 引数用の領域
        mov     edi, esp
        cld
        rep     movsb                   ;; [esi]->[edi] を ecx バイト
        call    ebx                     ;; wrapfn(...); eax = 戻り値
        lea     esp, [ebp - 12]         ;; コピー引数を破棄し保存レジスタ位置へ
        pop     ebx
        pop     edi
        pop     esi
        pop     ebp
        ret
