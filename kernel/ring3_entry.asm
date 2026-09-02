;; ============================================================
;; ring3_entry.asm — リング3 (CPL=3) システムコール入口 (v2 M1/M2)
;;
;; CPL=3 プログラムが `int 0x80` を実行するとここに入る。IDT ゲートは
;; DPL=3 (kernel/idt.c)。CPU は特権遷移で TSS.SS0:ESP0 のカーネルスタックに
;; 切り替え、SS/ESP/EFLAGS/CS/EIP を積む (エラーコードなし)。
;;
;; 呼出規約 (M1 最小版):
;;   eax = スロット番号 (M1 では未使用。M2 で KAPI トランポリンが使う)
;;   ebx = 引数0 (sys_exit の終了ステータス)
;;
;; M1 の唯一のシステムコールは sys_exit。C 側 ring3_syscall_dispatch() が
;; master PD へ戻し AS を破棄して longjmp するので、ここへは戻ってこない。
;; 将来 (M2) 戻り値のあるスロットを扱うときのために popad/iretd も置くが、
;; M1 経路では到達しない。
;; ============================================================

cpu 386

extern ring3_syscall_dispatch

section .text

;; C ハンドラはカーネルデータセグメントを前提にする。CPL=3 から来ると
;; DS/ES/FS/GS は USER_DS のままなので、C を呼ぶ前に必ず復元する
;; (isr_stub.asm の RESTORE_KSEG と同じ理由)。
global int80_stub
int80_stub:
        ;; ゲートは割込みゲートなので IF は既にクリア済み。
        mov     dx, 0x10            ;; KERNEL_DS
        mov     ds, dx
        mov     es, dx
        mov     fs, dx
        mov     gs, dx

        ;; cdecl 引数を積む: (slot, arg0)
        push    ebx                 ;; arg0 (status)
        push    eax                 ;; slot
        call    ring3_syscall_dispatch
        add     esp, 8

        ;; M1: sys_exit は戻らない。ここに来たら (将来のスロット) iretd で復帰。
        iretd
