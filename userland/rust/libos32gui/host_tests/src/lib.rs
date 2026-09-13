//! libos32gui のホスト TDD (票 S2-W、`tools/tests/s2_tdd.md` §W)。
//!
//! `../../src/cfgro.rs` をそのまま取り込み、C の `libos32cfg` の代わりに
//! 贋物を並べて **wrapper の分岐だけ**を固定する。SQLite も実 DB も出てこない
//! (それは S2-C の `test_cfg.py` の領分)。
//!
//! 見るもの:
//!   - get_int … open 失敗 / 未設定 / close 失敗 → すべて def
//!   - get_text … open 失敗はその値、NOTFOUND / NOSPC はそのまま、
//!     close だけ失敗なら IO、**直前の失敗を優先**
//!   - set_* … `app:` scope 以外は PERM で open すらしない、
//!     begin 失敗で rollback しない、set / commit 失敗で rollback する、
//!     close 失敗は IO、**直前の失敗を優先**

#[path = "../../src/cfgro.rs"]
pub mod cfgro;

pub mod fake;

#[cfg(test)]
mod tests {
    use super::cfgro::*;
    use super::fake::{self, Call};

    /* ---------------- 小道具 ---------------- */

    fn get_int(scope: &[u8], key: &[u8], def: i32) -> i32 {
        os32gui_cfg_get_int(
            scope.as_ptr(),
            scope.len() as u32,
            key.as_ptr(),
            key.len() as u32,
            def,
        )
    }

    fn get_text(scope: &[u8], key: &[u8], out: &mut [u8]) -> i32 {
        os32gui_cfg_get_text(
            scope.as_ptr(),
            scope.len() as u32,
            key.as_ptr(),
            key.len() as u32,
            out.as_mut_ptr(),
            out.len() as u32,
        )
    }

    fn set_int(scope: &[u8], key: &[u8], v: i32) -> i32 {
        os32gui_cfg_set_int(
            scope.as_ptr(),
            scope.len() as u32,
            key.as_ptr(),
            key.len() as u32,
            v,
        )
    }

    fn set_text(scope: &[u8], key: &[u8], s: &[u8]) -> i32 {
        os32gui_cfg_set_text(
            scope.as_ptr(),
            scope.len() as u32,
            key.as_ptr(),
            key.len() as u32,
            s.as_ptr(),
            s.len() as u32,
        )
    }

    /* ================================================================ */
    /*  純粋部: 分岐表と scope 検査                                      */
    /* ================================================================ */

    #[test]
    fn w01_fold_get_int_is_def_on_any_failure() {
        assert_eq!(fold_get_int(0, 5, 0, 7), 5, "全成功なら値");
        assert_eq!(fold_get_int(ERR_IO, 5, 0, 7), 7, "open 失敗 → def");
        assert_eq!(fold_get_int(0, 5, ERR_IO, 7), 7, "close 失敗 → def");
        assert_eq!(fold_get_int(0, -12345, 0, 7), -12345, "負の設定値は値として通す");
    }

    #[test]
    fn w02_fold_get_text_prefers_earlier_failure() {
        assert_eq!(fold_get_text(0, 4, 0), 4);
        assert_eq!(fold_get_text(ERR_INVAL, 0, 0), ERR_INVAL, "open 失敗はその値");
        assert_eq!(fold_get_text(0, ERR_NOTFOUND, 0), ERR_NOTFOUND);
        assert_eq!(fold_get_text(0, 0, ERR_IO), ERR_IO, "close だけ失敗 → IO");
        assert_eq!(
            fold_get_text(0, ERR_NOTFOUND, ERR_IO),
            ERR_NOTFOUND,
            "直前の失敗を優先"
        );
    }

    #[test]
    fn w03_fold_set_prefers_earlier_failure() {
        assert_eq!(fold_set(0, 0, 0), 0);
        assert_eq!(fold_set(ERR_IO, 0, 0), ERR_IO);
        assert_eq!(fold_set(0, ERR_INVAL, 0), ERR_INVAL);
        assert_eq!(fold_set(0, 0, ERR_IO), ERR_IO);
        assert_eq!(fold_set(0, ERR_INVAL, ERR_IO), ERR_INVAL, "直前の失敗を優先");
    }

    #[test]
    fn w04_scope_is_app_table() {
        assert!(scope_is_app(b"app:a"));
        assert!(scope_is_app(b"app:filer"));
        assert!(scope_is_app(b"app:my_app9"));
        assert!(!scope_is_app(b"app:"), "本体が空");
        assert!(!scope_is_app(b"system"));
        assert!(!scope_is_app(b"gshell"));
        assert!(!scope_is_app(b"user"));
        assert!(!scope_is_app(b""));
        assert!(!scope_is_app(b"App:x"), "大文字の接頭辞は不可");
        assert!(!scope_is_app(b"app:X"), "大文字の本体は不可");
        assert!(!scope_is_app(b"app:a-b"), "[a-z0-9_] 以外は不可");
        assert!(!scope_is_app(b"app:a/b"));
        assert!(!scope_is_app(b"xapp:a"));
        let mut long = b"app:".to_vec();
        long.resize(CFG_NAME_MAX, b'a');
        assert!(scope_is_app(&long), "63B ちょうどは通る");
        long.push(b'a');
        assert!(!scope_is_app(&long), "64B は不可");
    }

    #[test]
    fn w05_copy_cstr_and_copy_value() {
        let mut b = [0u8; CFG_NAME_CAP];
        assert!(copy_cstr(b"abc", &mut b));
        assert_eq!(&b[..4], b"abc\0");
        assert!(!copy_cstr(b"", &mut b), "空の scope / key は拒否");
        assert!(!copy_cstr(b"a\0b", &mut b), "埋め込み NUL は拒否");
        assert!(copy_cstr(&[b'x'; CFG_NAME_MAX], &mut b), "63B は通る");
        assert!(!copy_cstr(&[b'x'; CFG_NAME_CAP], &mut b), "64B は入らない");

        let mut v = [0u8; CFG_TEXT_CAP];
        assert!(copy_value(b"", &mut v), "空の**値**は許す (NULL とは別)");
        assert_eq!(v[0], 0);
        assert!(copy_value(&[b'x'; CFG_TEXT_MAX], &mut v), "255B は通る");
        assert!(!copy_value(&[b'x'; CFG_TEXT_CAP], &mut v), "256B は入らない");
        assert!(!copy_value(b"a\0b", &mut v));
    }

    /* ================================================================ */
    /*  get_int                                                         */
    /* ================================================================ */

    #[test]
    fn w06_get_int_reads_value_and_closes() {
        fake::reset();
        fake::set_get_int(42);
        assert_eq!(get_int(b"gshell", b"desktop/color", 7), 42);
        assert_eq!(
            fake::log(),
            vec![Call::Open(0), Call::GetInt, Call::Close],
            "open(RO) → get → close で閉じきる"
        );
        assert_eq!(fake::last_scope(), b"gshell");
        assert_eq!(fake::last_key(), b"desktop/color");
    }

    #[test]
    fn w07_get_int_open_failure_is_def() {
        fake::reset();
        fake::set_open_rc(ERR_IO);
        assert_eq!(get_int(b"gshell", b"k", 7), 7);
        assert_eq!(fake::log(), vec![Call::Open(0)], "open が負なら close しない");
    }

    #[test]
    fn w08_get_int_open_null_is_def() {
        fake::reset();
        fake::set_open_null(true);
        assert_eq!(get_int(b"gshell", b"k", 7), 7);
        assert_eq!(fake::log(), vec![Call::Open(0)], "NULL を掴まされたら触らない");
    }

    #[test]
    fn w09_get_int_notfound_is_def() {
        fake::reset();
        /* MISSING / CORRUPT / 未設定では cfg_get_int が def を返す契約。 */
        fake::set_get_int_passthrough_def(true);
        assert_eq!(get_int(b"gshell", b"k", 7), 7);
        assert_eq!(fake::log(), vec![Call::Open(0), Call::GetInt, Call::Close]);
    }

    #[test]
    fn w10_get_int_close_failure_is_def() {
        fake::reset();
        fake::set_get_int(42);
        fake::set_close_rc(ERR_IO);
        assert_eq!(get_int(b"gshell", b"k", 7), 7, "close 失敗なら読めた値も捨てる");
        assert_eq!(fake::log(), vec![Call::Open(0), Call::GetInt, Call::Close]);
    }

    #[test]
    fn w11_get_int_bad_name_never_opens() {
        fake::reset();
        fake::set_get_int(42);
        assert_eq!(get_int(b"", b"k", 7), 7, "空 scope");
        assert_eq!(get_int(b"gshell", b"", 7), 7, "空 key");
        assert_eq!(get_int(&[b'x'; 64], b"k", 7), 7, "64B の scope");
        assert_eq!(get_int(b"gshell", b"a\0b", 7), 7, "埋め込み NUL");
        assert!(fake::log().is_empty(), "検査で落ちたら DB を開かない");
    }

    /* ================================================================ */
    /*  get_text                                                        */
    /* ================================================================ */

    #[test]
    fn w12_get_text_reads_value() {
        fake::reset();
        fake::set_get_text(b"blue");
        let mut out = [0xAAu8; 16];
        assert_eq!(get_text(b"gshell", b"theme", &mut out), 4);
        assert_eq!(&out[..5], b"blue\0");
        assert_eq!(fake::log(), vec![Call::Open(0), Call::GetText, Call::Close]);
        assert_eq!(fake::last_cap(), 16);
    }

    #[test]
    fn w13_get_text_open_failure_passes_code_through() {
        fake::reset();
        fake::set_open_rc(ERR_INVAL);
        let mut out = [0u8; 16];
        assert_eq!(get_text(b"gshell", b"theme", &mut out), ERR_INVAL);
        assert_eq!(fake::log(), vec![Call::Open(0)]);
    }

    #[test]
    fn w14_get_text_notfound_and_nospc_pass_through() {
        fake::reset();
        fake::set_get_text_rc(ERR_NOTFOUND);
        let mut out = [0xAAu8; 16];
        assert_eq!(get_text(b"gshell", b"theme", &mut out), ERR_NOTFOUND);
        assert_eq!(out[0], 0xAA, "未設定なら out は書かない");

        fake::reset();
        const ERR_NOSPC: i32 = -4;
        fake::set_get_text_rc(ERR_NOSPC);
        assert_eq!(get_text(b"gshell", b"theme", &mut out), ERR_NOSPC);
        assert_eq!(out[0], 0xAA, "cap 不足でも out は書かない");
    }

    #[test]
    fn w15_get_text_close_failure_is_io() {
        fake::reset();
        fake::set_get_text(b"blue");
        fake::set_close_rc(ERR_IO);
        let mut out = [0u8; 16];
        assert_eq!(get_text(b"gshell", b"theme", &mut out), ERR_IO);
    }

    #[test]
    fn w16_get_text_get_failure_wins_over_close_failure() {
        fake::reset();
        fake::set_get_text_rc(ERR_NOTFOUND);
        fake::set_close_rc(ERR_IO);
        let mut out = [0u8; 16];
        assert_eq!(
            get_text(b"gshell", b"theme", &mut out),
            ERR_NOTFOUND,
            "直前の失敗を優先"
        );
    }

    #[test]
    fn w17_get_text_bad_args_never_open() {
        fake::reset();
        fake::set_get_text(b"blue");
        let mut out = [0u8; 16];
        assert_eq!(
            os32gui_cfg_get_text(b"gshell".as_ptr(), 6, b"k".as_ptr(), 1, core::ptr::null_mut(), 16),
            ERR_INVAL,
            "out が NULL"
        );
        assert_eq!(
            os32gui_cfg_get_text(b"gshell".as_ptr(), 6, b"k".as_ptr(), 1, out.as_mut_ptr(), 0),
            ERR_INVAL,
            "cap 0"
        );
        assert_eq!(get_text(b"", b"k", &mut out), ERR_INVAL, "空 scope");
        assert_eq!(get_text(b"gshell", b"", &mut out), ERR_INVAL, "空 key");
        assert!(fake::log().is_empty());
    }

    /* ================================================================ */
    /*  set_int / set_text                                              */
    /* ================================================================ */

    #[test]
    fn w18_set_refuses_os_scopes_without_opening() {
        fake::reset();
        for scope in [
            &b"system"[..],
            &b"gshell"[..],
            &b"user"[..],
            &b"app:"[..],
            &b"app:BAD"[..],
            &b""[..],
        ] {
            assert_eq!(set_int(scope, b"k", 1), ERR_PERM, "{:?}", scope);
            assert_eq!(set_text(scope, b"k", b"v"), ERR_PERM, "{:?}", scope);
        }
        assert!(fake::log().is_empty(), "拒否した要求で DB を開かない");
    }

    #[test]
    fn w19_set_int_writes_in_one_transaction() {
        fake::reset();
        assert_eq!(set_int(b"app:filer", b"pane/width", 120), 0);
        assert_eq!(
            fake::log(),
            vec![
                Call::Open(1),
                Call::Begin,
                Call::SetInt,
                Call::Commit,
                Call::Close
            ],
            "open(RW) → begin → set → commit → close を 1 呼び出しで閉じる"
        );
        assert_eq!(fake::last_scope(), b"app:filer");
        assert_eq!(fake::last_key(), b"pane/width");
        assert_eq!(fake::last_int(), 120);
    }

    #[test]
    fn w20_set_text_writes_value_including_empty() {
        fake::reset();
        assert_eq!(set_text(b"app:filer", b"last/path", b"/usr/bin"), 0);
        assert_eq!(fake::last_text(), b"/usr/bin");
        assert_eq!(
            fake::log(),
            vec![
                Call::Open(1),
                Call::Begin,
                Call::SetText,
                Call::Commit,
                Call::Close
            ]
        );

        fake::reset();
        assert_eq!(set_text(b"app:filer", b"last/path", b""), 0, "空値は書ける");
        assert_eq!(fake::last_text(), b"");
    }

    #[test]
    fn w21_set_text_rejects_oversized_value_after_scope_check() {
        fake::reset();
        let ok = vec![b'x'; CFG_TEXT_MAX];
        assert_eq!(set_text(b"app:filer", b"k", &ok), 0, "255B は通る");
        fake::reset();
        let ng = vec![b'x'; CFG_TEXT_MAX + 1];
        assert_eq!(set_text(b"app:filer", b"k", &ng), ERR_INVAL, "256B は INVAL");
        assert!(fake::log().is_empty(), "値の検査で落ちたら開かない");
    }

    #[test]
    fn w22_set_open_failure_passes_code_through() {
        fake::reset();
        fake::set_open_rc(ERR_IO);
        assert_eq!(set_int(b"app:filer", b"k", 1), ERR_IO);
        assert_eq!(fake::log(), vec![Call::Open(1)]);
    }

    #[test]
    fn w23_set_begin_failure_does_not_rollback() {
        fake::reset();
        /* MISSING / CORRUPT / VERSION は cfg_begin が INVAL を返す契約 (票 §1-1(d))。 */
        fake::set_begin_rc(ERR_INVAL);
        assert_eq!(set_int(b"app:filer", b"k", 1), ERR_INVAL);
        assert_eq!(
            fake::log(),
            vec![Call::Open(1), Call::Begin, Call::Close],
            "BEGIN していないので ROLLBACK しない"
        );
    }

    #[test]
    fn w24_set_failure_rolls_back_and_returns_its_code() {
        fake::reset();
        fake::set_set_rc(ERR_INVAL);
        assert_eq!(set_int(b"app:filer", b"k", 1), ERR_INVAL);
        assert_eq!(
            fake::log(),
            vec![
                Call::Open(1),
                Call::Begin,
                Call::SetInt,
                Call::Rollback,
                Call::Close
            ],
            "set が失敗したら commit せず rollback"
        );
    }

    #[test]
    fn w25_commit_failure_rolls_back_and_returns_its_code() {
        fake::reset();
        fake::set_commit_rc(ERR_IO);
        assert_eq!(set_int(b"app:filer", b"k", 1), ERR_IO);
        assert_eq!(
            fake::log(),
            vec![
                Call::Open(1),
                Call::Begin,
                Call::SetInt,
                Call::Commit,
                Call::Rollback,
                Call::Close
            ]
        );
    }

    #[test]
    fn w26_set_close_failure_is_io() {
        fake::reset();
        fake::set_close_rc(ERR_IO);
        assert_eq!(set_int(b"app:filer", b"k", 1), ERR_IO, "commit 済みでも close 失敗は隠さない");
        assert_eq!(
            fake::log(),
            vec![
                Call::Open(1),
                Call::Begin,
                Call::SetInt,
                Call::Commit,
                Call::Close
            ]
        );
    }

    #[test]
    fn w27_set_earlier_failure_wins_over_close_failure() {
        fake::reset();
        fake::set_set_rc(ERR_INVAL);
        fake::set_close_rc(ERR_IO);
        assert_eq!(
            set_int(b"app:filer", b"k", 1),
            ERR_INVAL,
            "直前の失敗を優先 (close の IO で上書きしない)"
        );
    }

    /* ================================================================ */
    /*  生の ptr + len の検査 (レビュー往復 1 の ⑮)                      */
    /* ================================================================ */

    #[test]
    fn w30_raw_span_ok_table() {
        let p = b"abc".as_ptr();
        assert!(raw_span_ok(p, 3, 63));
        assert!(raw_span_ok(p, 0, 63), "非 NULL + len 0 は空");
        assert!(!raw_span_ok(p, 64, 63), "上限超の len");
        assert!(!raw_span_ok(p, u32::MAX, 63), "巨大 len は slice を作る前に落とす");
        assert!(
            raw_span_ok(core::ptr::null(), 0, 63),
            "NULL + len 0 だけが空として通る"
        );
        assert!(
            !raw_span_ok(core::ptr::null(), 1, 63),
            "NULL + len != 0 を空スライスに化けさせない"
        );
        /* ptr + len が番地空間を折り返す (from_raw_parts の前提を破る)。 */
        let top = (usize::MAX - 3) as *const u8;
        assert!(!raw_span_ok(top, 8, 63));
    }

    #[test]
    fn w31_raw_out_ok_table() {
        let mut b = [0u8; 8];
        let p = b.as_mut_ptr();
        assert!(raw_out_ok(p, 8));
        assert!(!raw_out_ok(core::ptr::null(), 8), "NULL の書き込み先");
        assert!(!raw_out_ok(p, 0), "cap 0");
        assert!(!raw_out_ok(p, i32::MAX as u32 + 1), "C へ int で渡せない cap");
        assert!(!raw_out_ok((usize::MAX - 3) as *const u8, 8), "折り返し");
    }

    #[test]
    fn w32_set_text_null_value_with_nonzero_len_is_rejected() {
        /* 反例 (⑮): 有効な scope / key + `s = NULL, s_len = 1` が空スライスに
         * 化けると、既存値を**空 text で上書きして成功**してしまう。 */
        fake::reset();
        assert_eq!(
            os32gui_cfg_set_text(
                b"app:filer".as_ptr(),
                9,
                b"k".as_ptr(),
                1,
                core::ptr::null(),
                1
            ),
            ERR_INVAL
        );
        assert!(fake::log().is_empty(), "DB を開かない = 上書きしない");
    }

    #[test]
    fn w33_set_text_null_value_with_zero_len_is_the_empty_value() {
        fake::reset();
        assert_eq!(
            os32gui_cfg_set_text(
                b"app:filer".as_ptr(),
                9,
                b"k".as_ptr(),
                1,
                core::ptr::null(),
                0
            ),
            0,
            "NULL + len 0 は空値として書ける"
        );
        assert_eq!(fake::last_text(), b"");
    }

    #[test]
    fn w34_huge_len_is_rejected_before_slicing() {
        /* 非 NULL + 巨大長。素朴な実装だと 63 / 255B の拒否より前に
         * `from_raw_parts` の前提を破る。 */
        fake::reset();
        let sc = b"app:filer";
        let k = b"k";
        assert_eq!(
            os32gui_cfg_set_text(sc.as_ptr(), 9, k.as_ptr(), 1, b"v".as_ptr(), u32::MAX),
            ERR_INVAL,
            "値の長さ"
        );
        assert_eq!(
            os32gui_cfg_set_int(sc.as_ptr(), u32::MAX, k.as_ptr(), 1, 1),
            ERR_INVAL,
            "scope の長さ (scope_is_app より前)"
        );
        assert_eq!(
            os32gui_cfg_set_int(sc.as_ptr(), 9, k.as_ptr(), u32::MAX, 1),
            ERR_INVAL,
            "key の長さ"
        );
        assert_eq!(
            os32gui_cfg_get_int(sc.as_ptr(), u32::MAX, k.as_ptr(), 1, 7),
            7,
            "get_int は def"
        );
        let mut out = [0u8; 16];
        assert_eq!(
            os32gui_cfg_get_text(sc.as_ptr(), 9, k.as_ptr(), u32::MAX, out.as_mut_ptr(), 16),
            ERR_INVAL,
            "get_text は INVAL"
        );
        assert!(fake::log().is_empty());
    }

    #[test]
    fn w35_null_name_with_nonzero_len_is_rejected() {
        fake::reset();
        let mut out = [0u8; 16];
        let nul = core::ptr::null();
        assert_eq!(os32gui_cfg_get_int(nul, 1, b"k".as_ptr(), 1, 7), 7);
        assert_eq!(
            os32gui_cfg_get_text(nul, 1, b"k".as_ptr(), 1, out.as_mut_ptr(), 16),
            ERR_INVAL
        );
        assert_eq!(
            os32gui_cfg_set_int(nul, 1, b"k".as_ptr(), 1, 1),
            ERR_INVAL,
            "NULL scope は PERM ではなく INVAL (空スライスに化けない)"
        );
        assert_eq!(
            os32gui_cfg_set_int(b"app:filer".as_ptr(), 9, nul, 1, 1),
            ERR_INVAL,
            "NULL key"
        );
        assert!(fake::log().is_empty());
    }

    #[test]
    fn w28_set_bad_key_never_opens() {
        fake::reset();
        assert_eq!(set_int(b"app:filer", b"", 1), ERR_INVAL, "空 key");
        assert_eq!(set_int(b"app:filer", &[b'x'; 64], 1), ERR_INVAL, "64B の key");
        assert_eq!(set_int(b"app:filer", b"a\0b", 1), ERR_INVAL, "埋め込み NUL");
        assert!(fake::log().is_empty());
    }
}
