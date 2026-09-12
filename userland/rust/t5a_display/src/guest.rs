//! Guest-only glue. Host tests never call KAPI or the shared GUI library.
//!
//! 票 K6C-A: fixture 供給を **con_sink 供給**に置き換えた端末窓。待ちは
//! GetMessage 方式 (`libos32gui::run` の U3 ループ) のままで、吸い出しは
//! 反復タイマの中だけで行う。ここに busy loop は無い — 協調型なので回し
//! 続けると他のアプリが飢える。
use crate::{
    boundary, inject,
    input::{self, Action},
    launch::{self, Attach, Outcome, Step},
    paint,
    prompt::{self, Decision, Event, Mode, Next},
    session::Session,
    sink::{self, Record, Stop as SinkStop},
    state::{Fixture, Movement},
    status::{self, SinkStatus},
    storage::Storage,
    view::{Layout, MARGIN},
};
use libos32gui::gapi::{
    self,
    proto::{GuiEvent, GUI_COLOR_EDIT_BG, GUI_COLOR_TEXT, GUI_EV_TEXT},
    types::{Rect, Style, SurfaceId},
};
use libos32gui::{App, GuiErr, GuiResult, Timer, Ui, Window, WindowSpec};
use libos32term_render::{Glyphs, Rect as PixelRect, Sink, CELL_HEIGHT, CELL_WIDTH};
use os32api::KernelAPI;

static STORAGE: Storage = Storage::new();

/// 吸い出しタイマ。10ms 刻みなので 10 = 100ms (票 §1)。
const TIMER_SINK: u8 = 1;
const TIMER_TICKS: u16 = 10;

/// con_sink_read へ渡す私有バッファ。`cap >= CON_SINK_REC_MAX` (203) が
/// KAPI の要求で、下回ると `OS32_ERR_INVAL`。1KB あれば 1 回で 5 本以上入る。
const SINK_BUF: usize = 1024;
const _: () = assert!(SINK_BUF >= sink::REC_MAX);

/// タイマ 1 周で吸う上限 (票 §2-1)。リングは 8KB なので 1 周で汲み切れる。
/// 上限を置くのは、際限なく読み続けて Paint と他アプリを待たせないため。
const SINK_BUDGET: usize = 8 * 1024;

pub fn run(api: *mut KernelAPI) -> i32 {
    if api.is_null() {
        return GuiErr::INVAL.code();
    }
    // Claim before GUI/KAPI init: reentrant or repeated main cannot replace API
    // globals or construct a second mutable reference. Claim is never released.
    let cells = match STORAGE.take() {
        Some(c) => c,
        None => return GuiErr::INVAL.code(),
    };
    if let Err(e) = libos32gui::init(api) {
        return e.code();
    }
    let window = match build_window() {
        Ok(w) => w,
        Err(e) => return e.code(),
    };
    /* 空の画面で始める。以後は con_sink のレコードだけが入る (票 §2-4)。 */
    let session = match Session::new(cells, Fixture::Live) {
        Ok(s) => s,
        Err(_) => return GuiErr::INVAL.code(),
    };
    let mut app = DisplayApp {
        window: Some(window),
        _timer: None,
        session,
        buf: [0; SINK_BUF],
        sink: SinkStatus::default(),
        follow: true,
        runs: 0,
        paint_error: false,
        reader: false,
        /* 端末はプロンプトから始まる (子はまだいない)。 */
        mode: Mode::Prompt,
        line: prompt::Line::new(),
        attach: None,
    };
    /* 票 §5 R2: **イベントループ (とタイマ) に入る前に** con_sink_read を 1 回
     * 呼び、読み手権限を確立する。`kbd_inject` はこれを済ませた者しか受け付け
     * ない (kernel/kbd_inject.c の con_sink_reader_get 照合)。先客がいれば
     * OS32_ERR_EXIST が返り、以後この端末は**注入しない** — 打鍵は捨て、
     * 状態行は busy のままにする。ここで読めたレコードは捨てずに画面へ入れる。 */
    let _ = app.pump();
    app.reader = app.sink.error.is_none();

    /* タイマが張れなければ何も吸えない。同期で回す代案は取らない
     * (協調型なので他のアプリが止まる)。 */
    let timer = {
        let w = match app.window.as_ref() {
            Some(w) => w,
            None => return GuiErr::INVAL.code(),
        };
        match Timer::repeating(w, TIMER_SINK, TIMER_TICKS) {
            Ok(t) => t,
            Err(e) => return e.code(),
        }
    };
    app._timer = Some(timer);
    match libos32gui::run(&mut app) {
        Ok(()) => 0,
        Err(e) => e.code(),
    }
}

fn build_window() -> GuiResult<Window> {
    let info = gapi::screen_info();
    let plan = boundary::windows(info.width as i64, info.height as i64).ok_or(GuiErr::INVAL)?;
    let rect = gui_rect(plan[0]).ok_or(GuiErr::INVAL)?;
    Window::create(&WindowSpec::new(
        b"Terminal  Enter=run  exit/ESC=quit  UP DOWN ROLL",
        rect,
    ))
}

fn gui_rect(r: PixelRect) -> Option<Rect> {
    Some(Rect::new(
        i16::try_from(r.x0).ok()?,
        i16::try_from(r.y0).ok()?,
        i16::try_from(r.x1.checked_sub(r.x0)?).ok()?,
        i16::try_from(r.y1.checked_sub(r.y0)?).ok()?,
    ))
}
fn pixels(r: Rect) -> PixelRect {
    PixelRect {
        x0: r.x as i64,
        y0: r.y as i64,
        x1: r.x as i64 + r.w as i64,
        y1: r.y as i64 + r.h as i64,
    }
}

struct DisplayApp<'a> {
    window: Option<Window>,
    /// 持っているだけ。`Drop` が `kill_timer` を出す。
    _timer: Option<Timer>,
    session: Session<'a>,
    /// con_sink_read の行き先。毎周スタックに 1KB 積まないよう持ち回す。
    buf: [u8; SINK_BUF],
    sink: SinkStatus,
    /// 末尾追従。スクロール操作で切れ、末尾へ戻る操作で復活する。
    follow: bool,
    runs: u64,
    paint_error: bool,
    /// 票 §5 R2 の読み手権限を起動時に取れたか。**一度きりの判定**で、
    /// 後から反転させない (票 §6「失敗は状態行 busy のまま注入もしない」)。
    /// 偽なら打鍵は捨てる。
    reader: bool,
    /// 打鍵の行き先 (票 T7 E4)。`Prompt` のあいだは 1 バイトも注入しない。
    mode: Mode,
    /// プロンプトで編集中の行 (票 T7 E2)。
    line: prompt::Line,
    /// 要求表に積んだ 1 件 (票 T9 D4)。`Some` なら接続モード。子 ID と完了は
    /// con_sink の `EXIT` ではなく `launch_poll` で問い合わせる。
    attach: Option<Attach>,
}

impl DisplayApp<'_> {
    fn layout(&self) -> Option<Layout> {
        let (w, h) = self.window.as_ref()?.client_size();
        Layout::new(w as i64, h as i64)
    }
    fn fail(&mut self, ui: &mut Ui) {
        ui.quit();
    }
    fn repaint(&mut self, ui: &mut Ui) {
        if let Some(w) = self.window.as_ref() {
            if w.invalidate_all().is_err() {
                self.fail(ui);
            }
        }
    }

    /// 最下行だけ描き直す (票 E2)。打鍵のたびに全面を投げると 64 行ぶんの
    /// 描画が走り、協調型なので他のアプリまで待たせる (CLAUDE.md §4-25)。
    fn repaint_prompt(&mut self, ui: &mut Ui) {
        let Some(layout) = self.layout() else {
            self.repaint(ui);
            return;
        };
        if let Some(w) = self.window.as_ref() {
            let (cw, _) = w.client_size();
            let rect = Rect::new(0, layout.prompt_y() as i16, cw, CELL_HEIGHT as i16);
            if w.invalidate(rect).is_err() {
                self.fail(ui);
            }
        }
    }

    /// 端末が自分で出す 1 行を出力領域へ入れる (票 E3、con_sink は通らない)。
    /// 行の途中なら先に改行する — 子の出力に食い込ませない。
    fn echo(&mut self, msg: &prompt::Msg) {
        if self.session.display().terminal.model().state().cursor.0 != 0 {
            let _ = self.session.apply(Record::Print {
                color: 0,
                bytes: b"\n",
            });
        }
        let _ = self.session.apply(Record::Print {
            color: 0,
            bytes: msg.bytes(),
        });
        self.follow = true;
    }

    /// 候補パスが開けるか (票 E3 の存在確認)。`vfs_open` はディレクトリを
    /// 拒むので、`/usr` のようなパスは「無い」と同じ扱いになる。
    fn exists(path: &prompt::Path) -> bool {
        // SAFETY: libos32gui::init initialized os32api. The pointer is to a
        // private NUL-terminated buffer and sys_open only reads it (mode 0 =
        // O_RDONLY, sdk/rust/os32api/src/lib.rs fs::O_RDONLY).
        let fd = unsafe { (os32api::api().sys_open)(path.as_ptr(), 0) };
        if fd < 0 {
            return false;
        }
        // SAFETY: fd came from the sys_open above and is not used afterwards.
        unsafe {
            (os32api::api().sys_close)(fd);
        }
        true
    }

    /// OS32X ヘッダの宣言 (票 T8 D7)。開けない / 読めないものは
    /// [`prompt::Kind::Plain`] = 「止めない」に倒す (判定は `prompt::classify`)。
    fn header_kind(path: &prompt::Path) -> prompt::Kind {
        let mut hdr = [0u8; prompt::OS32X_HDR_SIZE];
        // SAFETY: libos32gui::init initialized os32api. The pointer is to a
        // private NUL-terminated buffer and sys_open only reads it (mode 0 =
        // O_RDONLY); hdr is a private buffer of exactly the length passed.
        let fd = unsafe { (os32api::api().sys_open)(path.as_ptr(), 0) };
        if fd < 0 {
            return prompt::Kind::Plain;
        }
        let n = unsafe {
            (os32api::api().sys_read)(fd, hdr.as_mut_ptr(), prompt::OS32X_HDR_SIZE as u32)
        };
        // SAFETY: fd came from the sys_open above and is not used afterwards.
        unsafe { (os32api::api().sys_close)(fd) };
        if n < prompt::OS32X_HDR_SIZE as i32 {
            return prompt::Kind::Plain;
        }
        prompt::classify(&hdr)
    }

    /// Enter で行を確定する (票 E3 / E5)。
    fn confirm(&mut self, ui: &mut Ui) {
        /* 行は Copy で持ち出す — 以後 self を触っても借りが残らない。 */
        let line = self.line;
        match prompt::decide(line.as_bytes()) {
            /* 空行はプロンプトを出し直すだけ (票 E5)。 */
            Decision::Empty => self.line.clear(),
            /* `exit` は ESC と同じく端末自身の終了 (票 E5)。 */
            Decision::Exit => {
                self.fail(ui);
                return;
            }
            Decision::Run { name, args } => self.launch(&line, name, args),
        }
        self.repaint(ui);
    }

    /// 候補を順に探し、見つかった絶対パスを**要求表**へ積む (票 E3 / T9 D4)。
    /// 取りに来て `exec_start` するのは WM (owner 1)。
    fn launch(&mut self, echo: &prompt::Line, name: &[u8], args: &[u8]) {
        /* 打った行は出力領域に残す — 接続モードではプロンプトが消えるので、
         * 何を走らせたのか分からなくなる。 */
        self.echo(&prompt::message(prompt::PREFIX, echo.as_bytes()));
        let candidates = prompt::candidates(name);
        let Some(path) = candidates.as_slice().iter().find(|p| Self::exists(p)) else {
            /* ローカル出力。con_sink は通らない (票 E3)。 */
            self.echo(&prompt::message(b"command not found: ", name));
            self.line.clear();
            return;
        };
        /* 入口 (票 T8 D7): OS32X ヘッダを読み、VRAM を直接触る CPL=0 プログラム
         * (v86 / VDM = `mkos32x --cpl0`) は GUI から起動しない。カーネルも GUI 中は
         * `OS32_ERR_INVAL` で拒むが、ここで止めれば接続モードにも入らずに済む。 */
        if Self::header_kind(path) == prompt::Kind::CuiOnly {
            /* ローカル出力。con_sink は通らない (票 E3)。 */
            self.echo(&prompt::message(b"cui only: ", name));
            self.line.clear();
            return;
        }
        let Some(cmd) = prompt::command_line(path, args) else {
            self.echo(&prompt::message(b"command line too long: ", name));
            self.line.clear();
            return;
        };
        /* 票 T9 D4: `session_launch` ではなく要求表へ積む。戻り値の token で
         * 子 ID と完了を問い合わせられる (`session_launch` は「1 本増やす」
         * だけで、どれが自分の子かも終わったかも分からなかった)。
         * SAFETY: libos32gui::init initialized os32api. cmd は NUL 終端の私有
         * バッファ (prompt::Path は常に len の次へ 0 を置く) で、カーネルは
         * 1〜255B を読むだけ (kapi_generated.rs: launch_req(*const u8) -> i32)。 */
        let rc = unsafe { (os32api::api().launch_req)(cmd.as_ptr()) };
        if rc > 0 {
            /* 受理された = 要求表に 1 本積んだ。WM が取って `exec_start` する。
             * token は**必ず**控える — 落とすと誰も poll せず、表が空かないまま
             * 次の `launch_req` が `OS32_ERR_FULL` で固着する。 */
            self.attach = Some(Attach::new(rc));
            /* 打鍵の行き先を子へ移す (D4)。 */
            if prompt::step(self.mode, Event::Launched) == Next::Attached {
                self.mode = Mode::Attached;
            }
            self.line.clear();
            return;
        }
        if rc == launch::ERR_FULL {
            /* 前の要求がまだ表に残っている。行は残して打ち直せるようにする
             * (票 E3 の `busy` と同じ扱い)。 */
            self.echo(&prompt::message(b"busy", b""));
            return;
        }
        /* それ以外は直らない要求 (宣言 LAUNCHER が無い / GUI 外 / 長すぎる)。
         * 行は消し、プロンプトのまま理由を出す (票 T9 D4)。 */
        self.echo(&prompt::message_rc(b"launch_req failed ", rc));
        self.line.clear();
    }

    /// 要求表を 1 回読む (票 T9 D4)。戻り値は「描き直す必要があるか」。
    fn poll_launch(&mut self) -> bool {
        let Some(mut attach) = self.attach else {
            return false;
        };
        let mut status: i32 = 0;
        // SAFETY: libos32gui::init initialized os32api. status is a local i32 and
        // the kernel only writes it on success (kapi_generated.rs:
        // launch_poll(i32, *mut i32) -> i32).
        let rc = unsafe { (os32api::api().launch_poll)(attach.token, &mut status) };
        let mut step = attach.poll(rc, status);
        /* 取消が `OS32_ERR_AGAIN` で止まっていたら、この周でもう一度出す
         * (票 D9: WM がまだ取っていないだけなので待てば通る)。 */
        if step == Step::Retry {
            step = self.cancel(&mut attach);
        }
        self.attach = Some(attach);
        match step {
            Step::Idle => false,
            Step::Redraw => true,
            /* cancel() は Retry を返さない (AGAIN は Redraw のまま次の周へ)。 */
            Step::Retry => true,
            Step::Finish(outcome) => {
                self.finish(outcome);
                true
            }
        }
    }

    /// `launch_cancel(token)` を 1 回出す (票 T9 D9)。
    fn cancel(&mut self, attach: &mut Attach) -> Step {
        // SAFETY: libos32gui::init initialized os32api. token is a plain i32
        // (kapi_generated.rs: launch_cancel(i32) -> i32).
        let rc = unsafe { (os32api::api().launch_cancel)(attach.token) };
        attach.cancelled(rc)
    }

    /// 接続モードを畳んでプロンプトへ戻す (票 T9 D4)。
    fn finish(&mut self, outcome: Outcome) {
        let event = match outcome {
            /* 子が終わった (取消で畳まれた場合も含む)。静かに戻る。 */
            Outcome::Done => Event::Done,
            Outcome::Failed(rc) => {
                self.echo(&prompt::message_rc(b"launch failed ", rc));
                Event::Failed
            }
            /* poll 自体が負 / 意味不明な status。黙って戻らず理由を出す ([V4])。 */
            Outcome::Lost(rc) => {
                self.echo(&prompt::message_rc(b"launch lost ", rc));
                Event::Failed
            }
        };
        self.attach = None;
        if prompt::step(self.mode, event) == Next::Prompt {
            self.mode = Mode::Prompt;
            self.line.clear();
        }
    }

    /// 接続モードで ESC を受けた (票 T9 D9)。`launch_cancel` を 1 回だけ出し、
    /// 接続モードのまま `DONE` を待つ (連打は `Attach` の印がまとめる)。
    fn escape(&mut self, ui: &mut Ui) {
        let Some(mut attach) = self.attach else {
            /* 表を持っていないのに接続モードに居る = 取りこぼし。固まらない
             * よう素直にプロンプトへ戻す。 */
            self.finish(Outcome::Done);
            self.repaint(ui);
            return;
        };
        if !attach.escape() {
            /* もう出してある。二重に出しても `AGAIN` / `STALE` が返るだけだが、
             * 呼ぶ回数は 1 回にまとめる (票 D9)。 */
            return;
        }
        let step = self.cancel(&mut attach);
        self.attach = Some(attach);
        match step {
            Step::Finish(outcome) => self.finish(outcome),
            /* 票 D9: 取消を出しても接続モードのまま (`Next::Stay`)。最下行が
             * `[cancelling …]` に変わり、プロンプトへは `DONE` を見てから。 */
            _ => {
                if prompt::step(self.mode, Event::CancelRequested) == Next::Prompt {
                    self.finish(Outcome::Done);
                }
            }
        }
        self.repaint(ui);
    }

    /// 最下行に出す接続モードの情報 (票 T9 D9)。
    fn running(&self) -> prompt::Running {
        match self.attach {
            Some(a) => prompt::Running {
                child: a.child,
                cancelling: a.cancelling(),
            },
            None => prompt::Running::default(),
        }
    }

    /// 表示の操作 (スクロール) と終了。注入とは無関係。
    fn navigate(&mut self, ui: &mut Ui, action: Action) {
        match action {
            Action::Quit => self.fail(ui),
            /* fixture の切り替えは live では意味を持たない (受け取った出力を
             * 捨てることになる)。ホスト試験だけが Select を使う。 */
            Action::Select(_) => {}
            Action::Move(movement) => {
                if let Some(layout) = self.layout() {
                    self.follow = matches!(movement, Movement::Last);
                    self.session.move_top(movement, layout.body_rows());
                    self.repaint(ui);
                }
            }
            Action::None => {}
        }
    }

    /// リングを空になるまで (1 周の予算まで) 吸って T4 モデルへ流す。
    /// 戻り値は「描き直す必要があるか」。
    fn pump(&mut self) -> bool {
        let mut changed = false;
        let mut budget = SINK_BUDGET;
        while !self.sink.stopped && budget >= SINK_BUF {
            // SAFETY: libos32gui::init initialized os32api. buf is a private,
            // writable SINK_BUF-byte array and cap matches its true length.
            let rc =
                unsafe { (os32api::api().con_sink_read)(self.buf.as_mut_ptr(), SINK_BUF as u32) };
            if rc < 0 {
                /* 黙って諦めない: 理由を状態行に出す。読み手拒否だけは先客が
                 * 畳めば直るので次の周も試す (票 §3 A4)。
                 * 描き直すのは理由が**変わった**ときだけ — 同じ拒否のたびに
                 * 全面 invalidate すると 10Hz で他のアプリを待たせる。 */
                let first = self.sink.error != Some(rc);
                self.sink.error = Some(rc);
                self.sink.stopped = !sink::retryable(rc);
                return changed || first;
            }
            if rc == 0 {
                if self.sink.error.is_some() {
                    /* 読めるようになった (先客が終わった)。 */
                    self.sink.error = None;
                    changed = true;
                }
                break;
            }
            let n = (rc as usize).min(SINK_BUF);
            self.sink.error = None;
            self.sink.bytes += n as u64;
            budget -= n;
            changed = true;

            let mut it = sink::records(&self.buf[..n]);
            /* `it` は self.buf を、apply は self.session と self.sink を触る。
             * 借りる先が別のフィールドなので同時に持てる。 */
            for record in it.by_ref() {
                self.sink.records += 1;
                /* 票 T9 D4: `EXIT` レコード (`Record::Exit`) は**表示にとどめ**、
                 * モード判定には使わない。子 ID と完了は要求表 (`launch_poll`)
                 * が答える — リングは drop-oldest なので制御情報は載せない
                 * (票 §7 blocker)。`session.apply` の `Exit` は何もしない。 */
                match self.session.apply(record) {
                    Ok(a) => {
                        self.sink.wraps += a.wraps;
                        self.sink.lost += a.lost;
                    }
                    Err(_) => {
                        /* モデルを作り直せない = 画面を持てない。以後読まない。 */
                        self.sink.stopped = true;
                        self.sink.error = Some(sink::ERR_INVAL);
                        break;
                    }
                }
            }
            if it.stop() != SinkStop::Done {
                self.sink.malformed += it.discarded() as u32;
            }
        }
        changed
    }

    /// 注入リングへ 1 回分積む (票 §5 R2 / §6 K7-A)。読み手になれていなければ
    /// **何もしない** — その打鍵は捨てる。戻り値は「状態行を描き直すか」。
    fn inject(&mut self, bytes: &inject::Bytes) -> bool {
        if !self.reader || bytes.is_empty() {
            return false;
        }
        let len = bytes.len();
        // SAFETY: libos32gui::init initialized os32api. The pointer is to a
        // private buffer with exactly `len` readable bytes and the kernel only
        // reads it (kapi_generated.rs: kbd_inject(*const u8, u32) -> i32).
        let rc = unsafe { (os32api::api().kbd_inject)(bytes.as_slice().as_ptr(), len as u32) };
        let error = if rc < 0 { Some(rc) } else { None };
        /* 0 <= rc < len は注入リングのあふれ = 消えた打鍵。累計で数える。 */
        let short = if rc >= 0 && (rc as usize) < len {
            (len - rc as usize) as u32
        } else {
            0
        };
        /* 描き直すのは状態が**変わった**ときだけ。同じ失敗のたびに全面
         * invalidate すると、打鍵のたびに他のアプリを待たせる (pump と同じ理由)。 */
        let changed = self.sink.inject_error != error || short != 0;
        self.sink.inject_error = error;
        self.sink.inject_short = self.sink.inject_short.saturating_add(short);
        changed
    }

    /// 溜まり具合と取りこぼしを読む (所有権は要らない)。
    fn refresh_stat(&mut self) -> bool {
        let mut ring: u32 = 0;
        let mut dropped: u32 = 0;
        // SAFETY: initialized KAPI; both pointers are to local u32s.
        unsafe {
            (os32api::api().con_sink_stat)(&mut ring, &mut dropped);
        }
        /* 描き直しを迫るのは dropped が増えたときだけ (票 §2-2)。ring は
         * 読めていない間ずっと動くので、これで描き直すと 10Hz で回り続ける。
         * 値は毎周更新してあるので、次の Paint で最新が出る。 */
        let changed = dropped != self.sink.dropped;
        self.sink.ring = ring;
        self.sink.dropped = dropped;
        changed
    }
}

impl App for DisplayApp<'_> {
    fn on_timer(&mut self, ui: &mut Ui, _window: u32, id: u8) {
        if id != TIMER_SINK || ui.is_quitting() {
            return;
        }
        let mut changed = self.pump();
        /* dropped が増えたら状態行に出す (票 §2-2)。増えた周は必ず描き直す。 */
        changed |= self.refresh_stat();
        /* 票 T9 D4: 同じ 100ms の周で要求表も 1 回読む (子 ID / 完了 / 失敗)。
         * 取消の再試行 (`OS32_ERR_AGAIN`) もここから出る (D9)。 */
        changed |= self.poll_launch();
        if !changed {
            return;
        }
        if self.follow {
            if let Some(layout) = self.layout() {
                self.session.move_top(Movement::Last, layout.body_rows());
            }
        }
        self.repaint(ui);
    }

    /// `GUI_EV_TEXT` はここにしか来ない (libos32gui はウィジェットへしか
    /// 配らない、`app.rs:257-262`)。FEP の確定文字を含む UTF-8 をそのまま
    /// 注入リングへ渡す (票 §1 D4)。**ローカルエコーはしない** — CUI
    /// プログラム側の出力が con_sink 経由で戻ってくる。
    fn on_raw(&mut self, ui: &mut Ui, ev: &GuiEvent) {
        if ev.kind != GUI_EV_TEXT || ui.is_quitting() {
            return;
        }
        if self.window.as_ref().map(Window::id) != Some(ev.window) {
            return;
        }
        let text = ev.text();
        let bytes = inject::from_text(ev.sub, &text.utf8);
        if self.mode == Mode::Prompt {
            /* 票 E2: プロンプト表示中は注入せず、行にそのまま足す (FEP の
             * 確定文字も同じ経路)。入り切らなければ黙って捨てる。 */
            let before = self.line.len();
            self.line.push(bytes.as_slice());
            if self.line.len() != before {
                self.repaint_prompt(ui);
            }
            return;
        }
        if self.inject(&bytes) {
            self.repaint(ui);
        }
    }

    fn on_key(&mut self, ui: &mut Ui, _window: u32, scan: u8, ch: u8, _mods: u8, down: bool) {
        if down && scan == libos32gui::widget::SCAN_ESC {
            /* 票 E4 / T9 D9: プロンプトの ESC は従来どおり自分の終了 (子が
             * 生きていても取消は出さない — 要求者の退場はカーネルが孤児回収
             * する)。接続モードの ESC は `launch_cancel` で、子には注がない。
             * プロンプトへは取消の `DONE` を poll で見てから戻る。 */
            match prompt::step(self.mode, Event::Escape) {
                Next::Quit => self.fail(ui),
                Next::Cancel => self.escape(ui),
                _ => {}
            }
            return;
        }
        if ui.is_quitting() {
            return;
        }
        if self.mode == Mode::Prompt {
            /* 票 E2: プロンプト表示中は 1 バイトも注入しない。 */
            if down {
                match scan & 0x7F {
                    inject::SCAN_RETURN => {
                        self.confirm(ui);
                        return;
                    }
                    inject::SCAN_BS => {
                        if self.line.backspace() {
                            self.repaint_prompt(ui);
                        }
                        return;
                    }
                    _ => {}
                }
            }
            /* 印字可能 ASCII と FEP の確定文字は GUI_EV_TEXT で来る (on_raw)。
             * ここでも拾うと 1 打鍵が 2 文字になる。残りは表示の操作だけ。 */
            self.navigate(ui, input::nav(scan, down));
            return;
        }
        /* 接続モード。制御キー (Enter / BS / TAB) だけ注ぐ。印字可能キーと
         * FEP の確定文字は GUI_EV_TEXT で来るので、ここで注ぐと 1 打鍵が
         * 2 バイトになる。 */
        if self.inject(&inject::from_key(scan, down)) {
            self.repaint(ui);
        }
        /* 表示の操作。注入が生きているあいだ ASCII の割り当て (j k g e q) は
         * 使わない — その打鍵は CUI プログラムのものだから。代わりに注入しない
         * キー (矢印 / ROLL / HOME) を使う。busy で始まった端末は表示専用なので
         * 従来どおり ASCII でも操作できる。 */
        let action = match input::nav(scan, down) {
            Action::None if !self.reader => input::key(ch, down),
            other => other,
        };
        self.navigate(ui, action);
    }

    fn on_close(&mut self, ui: &mut Ui, window: u32) {
        if self.window.as_ref().map(Window::id) == Some(window) {
            self.window = None;
            self.fail(ui);
        }
    }

    fn on_quit(&mut self, ui: &mut Ui, _reason: u8) {
        self.fail(ui);
    }

    fn on_paint(&mut self, _ui: &mut Ui, window: u32, surface: SurfaceId, rect: Rect) {
        let Some(owner) = self.window.as_ref().filter(|w| w.id() == window) else {
            return;
        };
        if owner.surface() != surface {
            return;
        }
        // libos32gui paint_damaged has installed a surface-specific base clip.
        // Intersect its current effective clip with this event; never install or
        // retain base clips ourselves. GUI coordinates are checked before calls.
        let clip = match pixels(rect).intersection(pixels(gapi::current_clip())) {
            Ok(Some(c)) => c,
            _ => return,
        };
        let (cw, ch) = owner.client_size();
        let Some(layout) = Layout::new(cw as i64, ch as i64) else {
            self.paint_error = true;
            let _ = owner.set_title(b"Terminal: client too small");
            return;
        };
        // This sink can only be constructed in this callback and never escapes.
        // paint::body preflights the full view/write extent before either trait
        // callback. hline itself takes i32; client/Rect coordinates fit i16.
        struct PaintSink {
            surface: SurfaceId,
        }
        impl Sink for PaintSink {
            fn run(&mut self, x: i32, y: i32, length: i32, foreground: bool) {
                let color = if foreground {
                    GUI_COLOR_TEXT
                } else {
                    GUI_COLOR_EDIT_BG
                };
                gapi::hline(self.surface, x, y, length, Style::pen(color));
            }
        }
        let mut sink = PaintSink { surface };
        match paint::body(
            self.session.display(),
            layout,
            clip,
            &mut GuestGlyphs,
            &mut sink,
        ) {
            Ok(stats) => {
                let lines = status::lines(
                    self.session.display(),
                    self.runs,
                    self.paint_error,
                    &self.sink,
                );
                let max_chars = ((cw as i64 - 2 * MARGIN) / CELL_WIDTH) as usize;
                for (row, line) in lines.iter().enumerate() {
                    let bytes = line.bytes();
                    gapi::text(
                        surface,
                        MARGIN as i32,
                        (MARGIN + row as i64 * CELL_HEIGHT) as i32,
                        &bytes[..bytes.len().min(max_chars)],
                        Style::new(GUI_COLOR_TEXT, GUI_COLOR_EDIT_BG),
                    );
                }
                /* 票 E2: 最下行は端末のもの。背景ごと描き直す — BS で縮んだ
                 * 残りが消えないと、消したはずの文字が見えたままになる。 */
                let row = prompt::row(
                    self.mode,
                    &self.line,
                    layout.prompt_cols(),
                    &self.running(),
                );
                let y = layout.prompt_y() as i32;
                gapi::fill_rect(
                    surface,
                    Rect::new(0, y as i16, cw, CELL_HEIGHT as i16),
                    Style::new(GUI_COLOR_TEXT, GUI_COLOR_EDIT_BG),
                );
                gapi::text(
                    surface,
                    MARGIN as i32,
                    y,
                    row.bytes(),
                    Style::new(GUI_COLOR_TEXT, GUI_COLOR_EDIT_BG),
                );
                self.runs = stats.runs;
            }
            Err(_) => {
                self.paint_error = true;
                let _ = owner.set_title(b"Terminal: PAINT RANGE/RENDER ERROR");
            }
        }
    }
}

extern "C" {
    // lib/utf8.h: u16 unicode_to_jis(u32 codepoint).
    // Final link MUST use lib/utf8_prog.o. It probes four known table entries
    // on first table use; do not call utf8_set_jis_table_ready(1).
    fn unicode_to_jis(codepoint: u32) -> u16;
}
struct GuestGlyphs;
impl Glyphs for GuestGlyphs {
    fn jis(&mut self, c: char) -> Option<u16> {
        // SAFETY: guest mapping and C ABI supplied by existing OS32 program path.
        boundary::valid_jis(unsafe { unicode_to_jis(c as u32) })
    }
    fn ank(&mut self, code: u8) -> [u8; 16] {
        let mut out = [0; 16];
        // SAFETY: libos32gui::init initialized os32api; writable 16-byte buffer
        // matches kapi_generated.rs. Void API cannot report missing glyphs.
        unsafe {
            (os32api::api().kcg_read_ank)(code, out.as_mut_ptr());
        }
        out
    }
    fn kanji(&mut self, code: u16) -> [u8; 32] {
        let mut out = [0; 32];
        if boundary::valid_jis(code).is_some() {
            // SAFETY: initialized KAPI, independently checked JIS bytes, writable
            // 32-byte buffer. [row*2]=left, [+1]=right; both MSB-left.
            unsafe {
                (os32api::api().kcg_read_kanji)(code, out.as_mut_ptr());
            }
        }
        // All-zero patterns are retained; the void API has no missing indicator.
        out
    }
}
