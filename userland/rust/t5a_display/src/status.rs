use crate::state::{Display, Fixture, Stop};
use core::fmt::{self, Write};
pub const LINE_COUNT: usize = 5;
pub struct Line {
    bytes: [u8; 64],
    len: usize,
}
impl Line {
    fn new() -> Self {
        Self {
            bytes: [0; 64],
            len: 0,
        }
    }
    pub fn bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }
}
impl Write for Line {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let end = self.len.checked_add(s.len()).ok_or(fmt::Error)?;
        let dst = self.bytes.get_mut(self.len..end).ok_or(fmt::Error)?;
        dst.copy_from_slice(s.as_bytes());
        self.len = end;
        Ok(())
    }
}
pub fn lines(s: &Display<'_>, runs: u64, paint_error: bool) -> [Line; LINE_COUNT] {
    let mut out: [Line; LINE_COUNT] = core::array::from_fn(|_| Line::new());
    let fixture = match s.fixture {
        Fixture::Normal => "1 NORMAL",
        Fixture::Exact => "2 EXACT",
        Fixture::Full => "3 FULL",
        Fixture::TooWide => "4 TOO WIDE",
        Fixture::Finish => "5 FINISH",
    };
    if paint_error {
        write!(out[0], "PAINT ERROR").unwrap();
    } else {
        write!(out[0], "{} ", fixture).unwrap();
        match s.stop {
            Stop::Complete => write!(out[0], "complete"),
            Stop::Feed(e) => write!(out[0], "feed:{:?}", e),
            Stop::Finish(e) => write!(out[0], "finish:{:?}", e),
        }
        .unwrap();
    }
    write!(out[1], "consumed={} bytes", s.consumed).unwrap();
    write!(out[2], "unconsumed={} bytes", s.total - s.consumed).unwrap();
    write!(out[3], "pending=").unwrap();
    for (i, pending) in s.terminal.pending().iter().enumerate() {
        if i != 0 {
            write!(out[3], ",").unwrap();
        }
        match pending {
            Some(c) => write!(out[3], "{:04X}", *c as u32),
            None => write!(out[3], "----"),
        }
        .unwrap();
    }
    write!(
        out[4],
        "top={} rows={} prev_runs={}",
        s.top,
        s.terminal.model().state().retained_rows.end,
        runs
    )
    .unwrap();
    out
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::state::CAPACITY;
    use libos32term::model::BLANK;
    #[test]
    fn stopped_status_distinguishes_bytes_pending_and_finish() {
        let mut cells = [BLANK; CAPACITY];
        let s = Display::load(&mut cells, Fixture::Full).unwrap();
        let l = lines(&s, 42, false);
        assert_eq!(l[0].bytes(), b"3 FULL feed:Full");
        assert_eq!(l[1].bytes(), b"consumed=2561 bytes");
        assert_eq!(l[2].bytes(), b"unconsumed=1 bytes");
        assert_eq!(l[3].bytes(), b"pending=0058,----");
        assert_eq!(l[4].bytes(), b"top=0 rows=64 prev_runs=42");
        drop(s);
        let s = Display::load(&mut cells, Fixture::Finish).unwrap();
        assert_eq!(lines(&s, 0, false)[0].bytes(), b"5 FINISH finish:Full");
        assert_eq!(lines(&s, 0, true)[0].bytes(), b"PAINT ERROR");
    }
}
