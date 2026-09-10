//! Owns the *external* backing borrow, never a buffer inside this struct.
use crate::state::{Display, Fixture, Movement};
use core::{marker::PhantomData, ptr::NonNull};
use libos32term::model::{Cell, Error};
pub struct Session<'a> {
    display: Option<Display<'a>>,
    backing: NonNull<[Cell]>,
    borrow: PhantomData<&'a mut [Cell]>,
}
impl<'a> Session<'a> {
    pub fn new(cells: &'a mut [Cell], fixture: Fixture) -> Result<Self, Error> {
        if cells.len() < crate::state::CAPACITY {
            return Err(Error::InsufficientStorage);
        }
        let backing = NonNull::from(cells);
        let mut session = Self {
            display: None,
            backing,
            borrow: PhantomData,
        };
        session.select(fixture)?;
        Ok(session)
    }
    // No mutable Display reference or owned Terminal can escape this wrapper.
    pub fn display(&self) -> &Display<'_> {
        self.display.as_ref().unwrap()
    }
    pub fn move_top(&mut self, movement: Movement, rows: usize) {
        self.display.as_mut().unwrap().move_top(movement, rows);
    }
    pub fn select(&mut self, fixture: Fixture) -> Result<(), Error> {
        self.display = None;
        // SAFETY: backing came from the sole &'a mut external slice in new().
        // The original reference is never reused. All Display access is shared
        // and tied to &self, so &mut self excludes outstanding display readers.
        // No Terminal / mutable Display escapes; the previous one was dropped
        // above. The slice stays alive for 'a via the exclusive PhantomData
        // borrow. This is a reborrow of external storage, not lifetime extension
        // of a field in a movable/self-referential struct.
        let cells = unsafe { &mut *self.backing.as_ptr() };
        self.display = Some(Display::load(cells, fixture)?);
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::state::{Stop, CAPACITY};
    use libos32term::model::BLANK;
    #[test]
    fn switching_drops_pending_limit_top_and_storage_contents() {
        let mut cells = [BLANK; CAPACITY];
        let mut session = Session::new(&mut cells, Fixture::Full).unwrap();
        session.move_top(Movement::Last, 10);
        session.select(Fixture::Normal).unwrap();
        assert_eq!(session.display().fixture, Fixture::Normal);
        assert_eq!(session.display().top, 0);
        assert_eq!(session.display().stop, Stop::Complete);
        assert_eq!(session.display().terminal.pending(), [None; 2]);
        assert_eq!(session.display().terminal.model().state().limit, None);
        assert_eq!(
            session.display().terminal.model().cells()[0],
            Cell::Single('A')
        );
        for _ in 0..20 {
            for f in [Fixture::Finish, Fixture::TooWide, Fixture::Normal] {
                session.select(f).unwrap();
                assert_eq!(session.display().fixture, f);
            }
        }
        assert_eq!(session.display().terminal.pending(), [None; 2]);
        assert_eq!(session.display().consumed, 229);
    }
}
