use libos32term_render::{Rect, View, CELL_HEIGHT, CELL_WIDTH};
pub const MARGIN: i64 = 8;
pub const BODY_Y: i64 = MARGIN + crate::status::LINE_COUNT as i64 * CELL_HEIGHT;
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Layout {
    width: i64,
    rows: usize,
}
impl Layout {
    pub fn new(width: i64, height: i64) -> Option<Self> {
        if !(2 * MARGIN + CELL_WIDTH..=i16::MAX as i64).contains(&width)
            || !(BODY_Y + CELL_HEIGHT + MARGIN..=i16::MAX as i64).contains(&height)
        {
            return None;
        }
        Some(Self {
            width,
            rows: ((height - BODY_Y - MARGIN) / CELL_HEIGHT) as usize,
        })
    }
    pub fn rows(self) -> usize {
        self.rows
    }
    pub fn view(self, top: usize, clip: Rect) -> Result<View, ()> {
        if clip.x0 < 0
            || clip.y0 < 0
            || clip.x1 > i16::MAX as i64
            || clip.y1 > i16::MAX as i64
            || clip.x1 < clip.x0
            || clip.y1 < clip.y0
        {
            return Err(());
        }
        let x0 = clip.x0.max(MARGIN);
        let y0 = clip.y0.max(BODY_Y);
        let clip = Rect {
            x0,
            y0,
            x1: clip.x1.min(self.width - MARGIN).max(x0),
            y1: clip.y1.min(BODY_Y + self.rows as i64 * CELL_HEIGHT).max(y0),
        };
        Ok(View {
            top,
            height: self.rows,
            origin: (MARGIN, BODY_Y),
            clip,
        })
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn client_rows_and_gui_preflight() {
        assert_eq!(Layout::new(340, 300).unwrap().rows, 12);
        for (w, h) in [
            (0, 300),
            (15, 300),
            (340, 103),
            (32768, 300),
            (340, i64::MAX),
        ] {
            assert_eq!(Layout::new(w, h), None);
        }
    }
    #[test]
    fn paint_intersects_body_and_rejects_bad_coordinates() {
        let l = Layout::new(340, 300).unwrap();
        let v = l
            .view(
                7,
                Rect {
                    x0: 3,
                    y0: 80,
                    x1: 20,
                    y1: 110,
                },
            )
            .unwrap();
        assert_eq!(
            v.clip,
            Rect {
                x0: 8,
                y0: 88,
                x1: 20,
                y1: 110
            }
        );
        assert_eq!(v.top, 7);
        assert!(l
            .view(
                0,
                Rect {
                    x0: 0,
                    y0: 0,
                    x1: i64::MAX,
                    y1: 2
                }
            )
            .is_err());
        assert!(l
            .view(
                0,
                Rect {
                    x0: 2,
                    y0: 0,
                    x1: 1,
                    y1: 2
                }
            )
            .is_err());
    }
}
