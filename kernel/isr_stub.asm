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
extern v86_gp_handler
extern v86_db_dispatch

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
;; #DB (例外1) は専用スタブを使用 (下記 db_stub)
ISR_NOERR 6                    ;; #UD 未定義命令
ISR_ERR   8                    ;; #DF ダブルフォルト
;; #PF は専用スタブを使用 (下記 pf_stub)

;; ============================================================
;; V86モード終了 共通サブルーチン
;;
;; #DB (isr_stub_1) と #GP (isr_stub_13) の V86 exit パスが
;; 同一コードのため共通化。
;; ============================================================
v86_exit_common:
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        extern  v86_test_exit
        call    v86_test_exit
        ;; ここには戻らない

;; ============================================================
;; #DB (例外1) 専用スタブ — V86 シングルステップ/ウォッチポイント
;;
;; TF (Trap Flag) による1命令シングルステップ (#DB) を処理する。
;; V86モードからの場合は v86_db_dispatch() に転送する。
;; 通常モードからの場合は isr_common に転送する。
;;
;; #DB はエラーコードなし。
;; スタック: [EIP] [CS] [EFLAGS] (V86: +[ESP][SS][ES][DS][FS][GS])
;; ============================================================
global isr_stub_1
isr_stub_1:
        cli
        ;; V86モード判定: EFLAGS.VM (bit 17)
        ;; [ESP+0]=EIP, [ESP+4]=CS, [ESP+8]=EFLAGS
        test    dword [esp + 8], 0x020000
        jnz     .v86_db

        ;; 通常の #DB 処理 → isr_common
        push    0               ;; ダミーエラーコード
        push    1               ;; 例外番号
        jmp     isr_common

.v86_db:
        ;; V86モードからの #DB
        ;; CPUスタックフレーム (エラーコードなし):
        ;;   [EIP(4)] [CS(4)] [EFLAGS(4)] [ESP(4)] [SS(4)] [ES(4)] [DS(4)] [FS(4)] [GS(4)]
        ;;
        ;; v86_gp_handler と同じレジスタ配列を作る:
        ;; ダミーエラーコードを挿入してから PUSHAD
        push    0               ;; ダミーエラーコード (GPフレームと互換)
        pushad                  ;; 汎用レジスタ保存 (32B)

        ;; DS/ES復元 (V86→Ring0遷移でCPUが0にクリア)
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax

        ;; v86_db_dispatch(u32 *regs) → 戻り値: 0=続行, 非0=V86終了
        mov     eax, esp
        push    eax
        call    v86_db_dispatch
        add     esp, 4

        ;; 戻り値チェック: 非0ならV86モードを完全終了
        test    eax, eax
        jnz     v86_exit_common

        ;; 通常復帰: V86に戻る
        popad
        add     esp, 4          ;; ダミーエラーコードをスキップ
        iretd                   ;; V86モードに復帰

;; ============================================================
;; #GP (例外13) 専用スタブ — V86モード判定付き
;;
;; V86モードからの#GPではCPUが自動的にセグメントレジスタを
;; スタックにpushするため、スタックフレームが異なる:
;;
;; 通常: [error_code] [EIP] [CS] [EFLAGS]
;; V86:  [error_code] [EIP] [CS] [EFLAGS] [ESP] [SS] [ES] [DS] [FS] [GS]
;;
;; EFLAGSのVMビット (bit 17) で判定する。
;; ============================================================
global isr_stub_13
isr_stub_13:
        cli
        ;; CPUが自動pushしたエラーコードの上にEIP, CS, EFLAGSがある
        ;; [ESP+0] = error_code
        ;; [ESP+4] = EIP
        ;; [ESP+8] = CS
        ;; [ESP+12] = EFLAGS
        test    dword [esp + 12], 0x020000   ;; EFLAGS.VM (bit 17) をテスト
        jnz     .v86_gp                      ;; V86モードなら専用パスへ

        ;; ★デバッグ: Ring0での#GPはESPを表示して停止
        ;; V86→Ring0遷移後のGP再発生ではDS/ES=0のため要復元
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax
        push    esp
        extern  print_debug_gp
        call    print_debug_gp
        add     esp, 4

        ;; 通常の#GP処理 (従来と同じ)
        push    13
        jmp     isr_common

.v86_gp:
        ;; ============================================================
        ;; V86モードからの#GP
        ;;
        ;; CPUが自動pushしたスタックフレーム (ESP低位→高位):
        ;;   [error_code(4)] [EIP(4)] [CS(4)] [EFLAGS(4)]
        ;;   [ESP(4)] [SS(4)] [ES(4)] [DS(4)] [FS(4)] [GS(4)]
        ;;
        ;; PUSHADで汎用レジスタも保存してからCハンドラに渡す。
        ;; ============================================================
        pushad                  ;; 汎用レジスタ保存 (32B)

        ;; ★ V86→Ring0遷移時にCPUがDS/ES/FS/GSを0にクリアするため、
        ;; Cハンドラ呼び出し前にカーネルデータセグメントを復元する。
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax

        ;; v86_gp_handler(u32 *regs) → 戻り値: 0=続行, 非0=V86終了
        mov     eax, esp
        push    eax
        call    v86_gp_handler
        add     esp, 4

        ;; 戻り値チェック: 非0ならV86モードを完全終了
        test    eax, eax
        jnz     v86_exit_common

        ;; 通常: v86_gp_handler がレジスタ配列を書き換えて戻った
        ;; (次に実行するV86命令のEIP等が更新済み)
        popad
        add     esp, 4          ;; error_code をスキップ
        iretd                   ;; V86モードに復帰 (CPUがセグメントも自動復帰)

;; ============================================================
;; 例外共通ハンドラ (V86以外)
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
;; V86モードからの#PFも検出する。
;; V86パスでは DS/ES 復元を追加するが、call先は同じ。
;;
;; CPU自動push後のスタック (ESP低位→高位):
;;   通常: [error_code] [EIP] [CS] [EFLAGS]
;;   V86:  [error_code] [EIP] [CS] [EFLAGS] [ESP] [SS] [ES] [DS] [FS] [GS]
;;
;; page_fault_handler(u32 error_code, u32 fault_addr, u32 fault_eip, u32 *regs)
;; regs[0]=EDI, [1]=ESI, [2]=EBP, [3]=ESP_orig, [4]=EBX, [5]=EDX, [6]=ECX, [7]=EAX
;; ============================================================
global isr_stub_14
isr_stub_14:
        cli
        ;; V86モード判定: EFLAGS.VM (bit 17) をチェック
        ;; [ESP+0]=error_code, [ESP+4]=EIP, [ESP+8]=CS, [ESP+12]=EFLAGS
        test    dword [esp + 12], 0x020000
        jnz     .v86_pf

        ;; 通常の#PF処理
        pushad                  ;; 全汎用レジスタ保存 (32B)
        jmp     .pf_common

.v86_pf:
        ;; V86モードからの#PF: DS/ES復元が必要
        pushad                  ;; 全汎用レジスタ保存 (32B)

        ;; ★ DS/ES復元 (V86→Ring0遷移でCPUが0にクリアするため)
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax

.pf_common:
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
;; IRQスタブ マクロ — DS/ES復元 + Cハンドラ呼び出し + EOI送出
;;
;; %1: IRQ名 (グローバルシンボル用)
;; %2: Cハンドラ名 (extern)
;; %3: 引数の有無 (0=引数なし, 1=regs(ESP)を引数で渡す)
;; %4: マスタのみか (0=マスタのみEOI, 1=スレーブ+マスタEOI)
;; ============================================================
%macro IRQ_STUB 4
global irq_stub_%1
irq_stub_%1:
        push    ds
        push    es
        pushad

        ;; ★ DS/ES復元 (V86モード対策)
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax

%if %3
        ;; 引数1: regs (ESP)
        mov     eax, esp
        push    eax
        call    %2
        add     esp, 4
%else
        call    %2
%endif

%if %4
        ;; スレーブPICにEOI送出 (PC-98: ポート 0x08)
        mov     al, OCW2_EOI
        out     PIC2_CMD, al
        ;; マスタPICにもEOI送出 (カスケード)
        out     PIC1_CMD, al
%else
        ;; マスタPICにEOI送出 (PC-98: ポート 0x00)
        mov     al, OCW2_EOI
        out     PIC1_CMD, al
%endif

        popad
        pop     es
        pop     ds
        iretd
%endmacro

;; ============================================================
;; IRQ0: タイマ割り込み (INT 0x20)
;; ※ tick_count インクリメントはマクロ外で行うため個別定義
;; ============================================================
global irq_stub_0
irq_stub_0:
        push    ds
        push    es
        pushad

        ;; ★ V86モードからの割り込み時、CPUがDS/ESを0にクリアするため復元
        mov     ax, 0x10
        mov     ds, ax
        mov     es, ax

        ;; tick_count をインクリメント
        inc     dword [tick_count]

        ;; 引数1: regs (ESP)
        mov     eax, esp
        push    eax
        call    timer_handler
        add     esp, 4

        ;; マスタPICにEOI送出 (PC-98: ポート 0x00)
        mov     al, OCW2_EOI
        out     PIC1_CMD, al

        popad
        pop     es
        pop     ds
        iretd

;; ============================================================
;; IRQ1: キーボード割り込み (INT 0x21) — マスタPIC
;; ============================================================
IRQ_STUB 1, kbd_irq_handler, 0, 0

;; ============================================================
;; IRQ4: RS-232C 割り込み (INT 0x24) — マスタPIC
;; ============================================================
IRQ_STUB 4, serial_irq_handler, 0, 0

;; ============================================================
;; IRQ7: スプリアス対策 (INT 0x27)
;; PC-98ではIR7がスレーブカスケードだが、スプリアスは起こりうる
;; ※ ISR読み出しが必要なため個別定義
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
;; IRQ11: FDD割り込み (INT 0x2B) — スレーブPIC
;; ============================================================
IRQ_STUB 11, fdc_irq_handler, 0, 1

;; ============================================================
;; IRQ13: マウス割り込み (INT 0x2D) — スレーブPIC
;; ============================================================
IRQ_STUB 13, mouse_irq_handler, 0, 1
