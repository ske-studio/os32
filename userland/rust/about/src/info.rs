//! info.rs — About に出す行の組み立て (KAPI にも GUI にも触らない純粋な部分)。
//!
//! 値の取得は `lib.rs` (KAPI と CPUID)、ここは**数値 → 表示の文字列**だけ。
//! ホスト試験 (`tools/tests/test_about_info.py`) がこのファイルをそのまま
//! `#[path]` で取り込んで回すので、`os32api` / `libos32gui` を use しない。
//!
//! 行はすべて ASCII (kanji の約束 — `utf8_set_jis_table_ready` — を持ち込まない)。
//! 1 行は [`LINE_MAX`] バイトで打ち切る (ASCII なので文字の途中では切れない)。

/// 1 行の最大バイト数。窓の外形は最大 440px、枠 2px × 2 と column の余白 10px × 2 を
/// 引いた 416px が 8px の半角で 52 桁。最長の行 ("CPU: " + ブランド文字列 47 桁) も 52。
pub const LINE_MAX: usize = 52;

/// `gfx_screen_info` の format (os32_kapi_shared.h `GFX_FMT_PACKED8`)。
pub const GFX_FMT_PACKED8: u8 = 1;
/// 能力ビット (os32_kapi_shared.h `GFX_CAP_HW_FILL` / `GFX_CAP_HW_BLT`)。
pub const GFX_CAP_HW_FILL: u32 = 0x0002;
pub const GFX_CAP_HW_BLT: u32 = 0x0004;

/// `BootImageInfo.source` (1 = FD ローダ、2 = HDD ローダ、0 = 不明)。
pub const BOOT_SRC_FD: u8 = 1;
pub const BOOT_SRC_HDD: u8 = 2;

/* ================================================================ */
/*  1 行ぶんの書き込み先                                             */
/* ================================================================ */

/// 固定長の 1 行。あふれた分は捨てる (ASCII だけを入れる)。
pub struct Line {
    buf: [u8; LINE_MAX],
    n: usize,
}

impl Line {
    pub const fn new() -> Line {
        Line { buf: [0; LINE_MAX], n: 0 }
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.n]
    }

    /// バイト列を足す。NUL が来たらそこで止める (KAPI の C 文字列をそのまま渡せる)。
    pub fn s(&mut self, t: &[u8]) -> &mut Line {
        let mut i = 0;
        while i < t.len() && t[i] != 0 && self.n < LINE_MAX {
            self.buf[self.n] = t[i];
            self.n += 1;
            i += 1;
        }
        self
    }

    /// 10 進。
    pub fn dec(&mut self, v: u32) -> &mut Line {
        let mut tmp = [0u8; 10];
        let mut k = 0;
        let mut x = v;
        loop {
            tmp[k] = b'0' + (x % 10) as u8;
            k += 1;
            x /= 10;
            if x == 0 {
                break;
            }
        }
        while k > 0 {
            k -= 1;
            self.s(&tmp[k..k + 1]);
        }
        self
    }

    /// 16 進 8 桁 (小文字、`ver` の `%08x` と同じ)。
    pub fn hex8(&mut self, v: u32) -> &mut Line {
        let digits = b"0123456789abcdef";
        let mut sh: i32 = 28;
        while sh >= 0 {
            let d = ((v >> sh) & 0xF) as usize;
            self.s(&digits[d..d + 1]);
            sh -= 4;
        }
        self
    }
}

/* ================================================================ */
/*  SYS_VERSION (include/config.h が唯一の定義)                      */
/* ================================================================ */

/// `#define SYS_VERSION "x.y"` の引用符の中身を返す。見つからなければ `b"?"`。
///
/// `ver` はカーネル側で同じマクロを表示する。版を 2 か所に書かないために、
/// About は config.h 自体を取り込んで**コンパイル時に**ここで切り出す
/// (`const fn`、実行時に config.h を抱えない)。
pub const fn sys_version(cfg: &[u8]) -> &[u8] {
    let key = b"#define SYS_VERSION";
    let mut i = 0;
    while i + key.len() <= cfg.len() {
        /* 行頭だけを見る (コメントの中の同名は拾わない)。 */
        if i == 0 || cfg[i - 1] == b'\n' {
            let mut k = 0;
            while k < key.len() && cfg[i + k] == key[k] {
                k += 1;
            }
            if k == key.len() {
                let mut p = i + k;
                /* 名前の直後は空白でなければ別のマクロ (SYS_VERSION_X など)。 */
                if p < cfg.len() && (cfg[p] == b' ' || cfg[p] == b'\t') {
                    while p < cfg.len() && (cfg[p] == b' ' || cfg[p] == b'\t') {
                        p += 1;
                    }
                    if p < cfg.len() && cfg[p] == b'"' {
                        let s = p + 1;
                        let mut e = s;
                        while e < cfg.len() && cfg[e] != b'"' && cfg[e] != b'\n' {
                            e += 1;
                        }
                        if e < cfg.len() && cfg[e] == b'"' && e > s {
                            let (_, tail) = cfg.split_at(s);
                            let (v, _) = tail.split_at(e - s);
                            return v;
                        }
                    }
                }
            }
        }
        i += 1;
    }
    b"?"
}

/* ================================================================ */
/*  各行                                                             */
/* ================================================================ */

/// "OS32 v2.0" (`ver` の 1 行目と同じ版の文字列)。
pub fn title_line(out: &mut Line, version: &[u8]) {
    out.s(b"OS32 v").s(version);
}

/// "Build: <日時>" (`sys_get_build_info`、カーネルを組んだ日時)。
pub fn build_line(out: &mut Line, build: &[u8]) {
    out.s(b"Build: ").s(build);
}

/// "Commit: <id>" (`BootImageInfo.commit`)。取れなければ "unknown"。
pub fn commit_line(out: &mut Line, ok: bool, commit: &[u8]) {
    out.s(b"Commit: ");
    if ok && !commit.is_empty() && commit[0] != 0 {
        out.s(commit);
    } else {
        out.s(b"unknown");
    }
}

/// "Image CRC: xxxxxxxx (N bytes)" / "Image CRC: none"。
pub fn crc_line(out: &mut Line, ok: bool, crc_valid: bool, crc: u32, size: u32) {
    out.s(b"Image CRC: ");
    if ok && crc_valid {
        out.hex8(crc).s(b" (").dec(size).s(b" bytes)");
    } else {
        out.s(b"none");
    }
}

/// "KernelAPI: vN"。
pub fn kapi_line(out: &mut Line, version: u32) {
    out.s(b"KernelAPI: v").dec(version);
}

/// "Boot: HDD" / "Boot: FD" / "Boot: unknown" (`BootImageInfo.source`)。
pub fn boot_line(out: &mut Line, ok: bool, source: u8) {
    out.s(b"Boot: ");
    out.s(if !ok {
        b"unknown".as_slice()
    } else if source == BOOT_SRC_HDD {
        b"HDD".as_slice()
    } else if source == BOOT_SRC_FD {
        b"FD".as_slice()
    } else {
        b"unknown".as_slice()
    });
}

/// "Memory: N KB (M MB)" — `sys_ram_kb` (起動時に登録した RAM の合計。
/// 15-16MB のシステム空間を含まない、`mem` の RAM 行と同じ値)。
pub fn mem_line(out: &mut Line, ram_kb: u32) {
    out.s(b"Memory: ").dec(ram_kb).s(b" KB (").dec(ram_kb / 1024).s(b" MB)");
}

/// "Kernel heap: F KB free of T KB" — `kmalloc_free` / `kmalloc_total` (バイト)。
/// 物理ページの空きを返す KAPI は無いので、「空き」はカーネルヒープで示す。
pub fn heap_line(out: &mut Line, free_b: u32, total_b: u32) {
    out.s(b"Kernel heap: ")
        .dec(free_b / 1024)
        .s(b" KB free of ")
        .dec(total_b / 1024)
        .s(b" KB");
}

/// バックエンドの名前 (`gshell` / `hal_test` / `gfxmode` と同じ区別)。
/// パックド 8bpp で HW_FILL / HW_BLT が立つのはアクセラレータ (Cirrus) だけ。
pub fn backend_name(format: u8, flags: u32) -> &'static [u8] {
    if format != GFX_FMT_PACKED8 {
        b"PC-98"
    } else if (flags & (GFX_CAP_HW_FILL | GFX_CAP_HW_BLT)) != 0 {
        b"Cirrus"
    } else {
        b"PEGC"
    }
}

/// 色数 (bpp から。0 や 16 超は数えずに "?" を出す側で扱う)。
pub fn colors(bpp: u8) -> u32 {
    if bpp == 0 || bpp > 16 {
        0
    } else {
        1u32 << bpp
    }
}

/// "Display: PEGC 640x480, 256 colors"。
pub fn display_line(out: &mut Line, width: u16, height: u16, bpp: u8, format: u8, flags: u32) {
    out.s(b"Display: ")
        .s(backend_name(format, flags))
        .s(b" ")
        .dec(width as u32)
        .s(b"x")
        .dec(height as u32)
        .s(b", ");
    let c = colors(bpp);
    if c == 0 {
        out.s(b"? colors");
    } else {
        out.dec(c).s(b" colors");
    }
}

/* ================================================================ */
/*  CPU                                                              */
/* ================================================================ */

/// CPU の識別結果 (lib.rs の `cpu::probe` が埋める)。
pub struct Cpu {
    /// 3 = CPUID 無し・AC ビット無し (386)、4 = CPUID 無し・AC あり (初期の 486)、
    /// 0 = CPUID あり (下の欄が有効)。
    pub class: u8,
    pub vendor: [u8; 12],
    pub family: u32,
    pub model: u32,
    /// 拡張 0x80000002..4 のブランド文字列 (NUL 終端、無ければ先頭 0)。
    pub brand: [u8; 48],
}

impl Cpu {
    pub const fn empty() -> Cpu {
        Cpu { class: 0, vendor: [0; 12], family: 0, model: 0, brand: [0; 48] }
    }
}

/// "CPU: <ブランド文字列>" / "CPU: GenuineIntel family 5 model 2" /
/// "CPU: 386 (no CPUID)" / "CPU: 486 (no CPUID)"。
pub fn cpu_line(out: &mut Line, c: &Cpu) {
    out.s(b"CPU: ");
    if c.class == 3 {
        out.s(b"386 (no CPUID)");
        return;
    }
    if c.class == 4 {
        out.s(b"486 (no CPUID)");
        return;
    }
    /* ブランド文字列は Intel が右寄せで先頭に空白を詰めることがある。 */
    let mut b = 0;
    while b < c.brand.len() && c.brand[b] == b' ' {
        b += 1;
    }
    if b < c.brand.len() && c.brand[b] != 0 {
        out.s(&c.brand[b..]);
        return;
    }
    out.s(&c.vendor).s(b" family ").dec(c.family).s(b" model ").dec(c.model);
}

/// CPUID leaf 1 の EAX から (family, model) を出す (拡張ファミリ・モデル込み)。
pub fn family_model(eax: u32) -> (u32, u32) {
    let base_family = (eax >> 8) & 0xF;
    let base_model = (eax >> 4) & 0xF;
    let mut family = base_family;
    let mut model = base_model;
    if base_family == 0xF {
        family = base_family + ((eax >> 20) & 0xFF);
    }
    if base_family == 0x6 || base_family == 0xF {
        model = base_model | (((eax >> 16) & 0xF) << 4);
    }
    (family, model)
}
