;; ============================================================
;; v86_entry.asm - V86モード遷移・復帰アセンブリ
;;
;; V86モードへの遷移は IRETD で行う。
;; EFLAGSのVMビット (bit 17) を立てた状態でIRETすると
;; CPUは仮想8086モードに移行する。
;;
;; V86モード内で特権命令 (INT/CLI/STI/IN/OUT等) を実行すると
;; #GP (例外13) が発生し、OS32のRing0ハンドラに制御が移る。
;; ============================================================

cpu 386

;; EFLAGS定数
EFLAGS_IF   equ 0x0200      ;; 割り込み許可
EFLAGS_VM   equ 0x020000    ;; 仮想8086モード
EFLAGS_IOPL0 equ 0x0000     ;; IOPL=0 (全I/Oトラップ ... I/Oビットマップ参照)

;; V86コンテキスト構造体 (Cから渡される)
;; struct v86_context {
;;     u32 eip;     /* +0  : V86エントリポイント */
;;     u32 cs;      /* +4  : V86コードセグメント */
;;     u32 eflags;  /* +8  : EFLAGS (VM=1, IF=1) */
;;     u32 esp;     /* +12 : V86スタックポインタ */
;;     u32 ss;      /* +16 : V86スタックセグメント */
;;     u32 es;      /* +20 : ES */
;;     u32 ds;      /* +24 : DS */
;;     u32 fs;      /* +28 : FS */
;;     u32 gs;      /* +32 : GS */
;; };

section .text

;; ============================================================
;; v86_enter - V86モードへ遷移
;;
;; void v86_enter(const struct v86_context *ctx);
;;
;; IRETDスタックフレーム (V86モード遷移時、低位→高位):
;;   GS, FS, DS, ES, SS, ESP, EFLAGS(VM=1), CS, EIP
;;
;; 戻り値: なし (V86内で#GPが発生するまで戻らない)
;; ============================================================
global v86_enter
v86_enter:
        ;; 引数: ESP+4 = ctx ポインタ
        mov     esi, [esp + 4]

        ;; V86 IRETD スタックフレームを構築
        push    dword [esi + 32]    ;; GS
        push    dword [esi + 28]    ;; FS
        push    dword [esi + 24]    ;; DS
        push    dword [esi + 20]    ;; ES
        push    dword [esi + 16]    ;; SS
        push    dword [esi + 12]    ;; ESP
        push    dword [esi + 8]     ;; EFLAGS (VM=1, IF=1)
        push    dword [esi + 4]     ;; CS
        push    dword [esi + 0]     ;; EIP

        ;; V86モードに遷移
        iretd

;; ============================================================
;; v86_return_point - V86からの復帰地点
;;
;; #GPハンドラがV86終了を検知した場合、ここにジャンプして
;; 呼び出し元に戻る。
;; (実際の復帰はsetjmp/longjmpで行うため、このラベルは予備)
;; ============================================================
global v86_return_point
v86_return_point:
        ret
