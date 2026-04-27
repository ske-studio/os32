;; ============================================================
;; isr_stub.asm — 割り込みハンドラASMスタブ (32ビット)
;;
;; CPU例外とIRQの割り込みを受け取り、Cハンドラにディスパッチする。
;; 全レジスタを保存・復帰し、IRETDで復帰する。
;; ============================================================

cpu 386

;; テキストVRAM直接書き込み用定数
TVRAM_CHAR  equ 0xA0000
TVRAM_ATTR  equ 0xA2000

;; PICポート (PC-98)
PIC1_CMD    equ 0x00
PIC2_CMD    equ 0x08
OCW2_EOI    equ 0x20

;; 外部Cハンドラ (GCC ELFではアンダースコアなし)
extern exception_handler
extern page_fault_handler
extern timer_handler
extern kbd_irq_handler
extern serial_irq_handler
extern tick_count
extern fdc_irq_handler
extern mouse_irq_handler

section .text

;; ============================================================
;; CPU例外スタブ: 例外番号をpushしてからCハンドラへ
;; ============================================================

;; マクロ: エラーコードなし例外
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
        cli
        push    0               ;; ダミーエラーコード
        push    %1              ;; 例外番号
        jmp     isr_common
%endmacro

;; マクロ: エラーコードあり例外
%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
        cli
        ;; エラーコードはCPUが自動push済み
        push    %1              ;; 例外番号
        jmp     isr_common
%endmacro

;; 例外スタブ生成
ISR_NOERR 0                    ;; #DE ゼロ除算
ISR_NOERR 6                    ;; #UD 未定義命令
ISR_ERR   8                    ;; #DF ダブルフォルト
ISR_ERR   13                   ;; #GP 一般保護例外
;; #PF は専用スタブを使用 (下記 pf_stub)

;; ============================================================
;; 例外共通ハンドラ
;; pushad後のスタック (低→高):
;;   [PUSHAD(32)] [vector(4)] [error_code(4)] [EIP(4)] [CS(4)] [EFLAGS(4)]
;;
;; exception_handler(u32 error_code, u32 vector, u32 fault_eip, u32 *regs)
;; ============================================================
isr_common:
        pushad                  ;; 全汎用レジスタ保存 (32B)
                                ;; ESP = base とする
                                ;; base+0..31: PUSHAD
                                ;; base+32: vector
                                ;; base+36: error_code
                                ;; base+40: fault EIP

        ;; 引数4: regs (PUSHAD配列先頭)
        mov     eax, esp
        push    eax             ;; ESP=base-4

        ;; 引数3: fault_eip
        mov     eax, [esp + 4 + 32 + 4 + 4] ;; base-4+44 = base+40 ✓
        push    eax             ;; ESP=base-8

        ;; 引数2: vector
        mov     eax, [esp + 8 + 32]          ;; base-8+40 = base+32 ✓
        push    eax             ;; ESP=base-12

        ;; 引数1: error_code
        mov     eax, [esp + 12 + 32 + 4]     ;; base-12+48 = base+36 ✓
        push    eax             ;; ESP=base-16

        call    exception_handler
        add     esp, 16

        popad
        add     esp, 8          ;; error_code + vector をスキップ
        iretd

;; ============================================================
;; #PF ページフォルト専用スタブ (ISR 14)
;;
;; CPUが自動pushするエラーコードに加え、
;; CR2 (障害アドレス) をCハンドラに渡す。
;;
;; CPU自動push後のスタック (ESP低位→高位):
;;   [error_code] [EIP] [CS] [EFLAGS]
;;
;; page_fault_handler(u32 error_code, u32 fault_addr, u32 fault_eip, u32 *regs)
;; regs[0]=EDI, [1]=ESI, [2]=EBP, [3]=ESP_orig, [4]=EBX, [5]=EDX, [6]=ECX, [7]=EAX
;; ============================================================
global isr_stub_14
isr_stub_14:
        cli
        pushad                  ;; 全汎用レジスタ保存 (32B)

        ;; PUSHAD配列のポインタ (引数4: regs)
        mov     eax, esp
        push    eax             ;; 引数4: regs (PUSHAD配列先頭)

        ;; フォルト時EIP取得 (PUSHAD=32B + push×1=4B + error_code=4B の上)
        mov     eax, [esp + 40] ;; EIP
        push    eax             ;; 引数3: fault_eip

        ;; CR2 (障害アドレス) 取得
        mov     eax, cr2
        push    eax             ;; 引数2: fault_addr (CR2)

        ;; エラーコード取得 (PUSHAD=32B + push×3=12B の上)
        mov     eax, [esp + 44] ;; error_code
        push    eax             ;; 引数1: error_code

        call    page_fault_handler
        add     esp, 16

        popad
        add     esp, 4          ;; error_code をスキップ
        iretd

;; ============================================================
;; デフォルトハンドラ (何もせずIRETD)
;; ============================================================
global isr_stub_default
isr_stub_default:
        iretd

;; ============================================================
;; IRQ0: タイマ割り込み (INT 0x20)
;; ============================================================
global irq_stub_0
irq_stub_0:
        pushad

        ;; tick_count をインクリメント
        inc     dword [tick_count]

        ;; Cハンドラを呼び出し
        call    timer_handler

        ;; マスタPICにEOI送出 (PC-98: ポート 0x00)
        mov     al, OCW2_EOI
        out     PIC1_CMD, al

        popad
        iretd

;; ============================================================
;; IRQ1: キーボード割り込み (INT 0x21)
;; ============================================================
global irq_stub_1
irq_stub_1:
        pushad

        ;; Cハンドラを呼び出し
        call    kbd_irq_handler

        ;; マスタPICにEOI送出 (PC-98: ポート 0x00)
        mov     al, OCW2_EOI
        out     PIC1_CMD, al

        popad
        iretd

;; ============================================================
;; IRQ4: RS-232C 割り込み (INT 0x24)
;; ============================================================
global irq_stub_4
irq_stub_4:
        pushad

        ;; Cハンドラを呼び出し
        call    serial_irq_handler

        ;; マスタPICにEOI送出 (PC-98: ポート 0x00)
        mov     al, OCW2_EOI
        out     PIC1_CMD, al

        popad
        iretd

;; ============================================================
;; IRQ7: スプリアス対策 (INT 0x27)
;; PC-98ではIR7がスレーブカスケードだが、スプリアスは起こりうる
;; ============================================================
global irq_stub_7
irq_stub_7:
        push    eax
        ;; ISR読み出しでスプリアスか確認
        mov     al, 0x0B        ;; OCW3: ISR読み出し指定
        out     PIC1_CMD, al
        in      al, PIC1_CMD
        test    al, 0x80        ;; IR7がセットされているか
        jnz     .real            ;; 本物の割り込みなら処理

        ;; スプリアス → EOIを送らずに無視
        pop     eax
        iretd

.real:
        ;; 本物のIR7割り込み (スレーブカスケード等)
        mov     al, OCW2_EOI
        out     PIC1_CMD, al
        pop     eax
        iretd

;; ============================================================
;; IRQ11: FDD割り込み (INT 0x2B) — スレーブPIC IR11
;; ============================================================
global irq_stub_11
irq_stub_11:
        pushad

        ;; Cハンドラを呼び出し
        call    fdc_irq_handler

        ;; スレーブPICにEOI送出 (PC-98: ポート 0x08)
        mov     al, OCW2_EOI
        out     PIC2_CMD, al
        ;; マスタPICにもEOI送出 (カスケード)
        out     PIC1_CMD, al

        popad
        iretd

;; ============================================================
;; IRQ13: マウス割り込み (INT 0x2D) — スレーブPIC IR5
;; ============================================================
global irq_stub_13
irq_stub_13:
        pushad

        ;; Cハンドラを呼び出し
        call    mouse_irq_handler

        ;; スレーブPICにEOI送出 (PC-98: ポート 0x08)
        mov     al, OCW2_EOI
        out     PIC2_CMD, al
        ;; マスタPICにもEOI送出 (カスケード)
        out     PIC1_CMD, al

        popad
        iretd
