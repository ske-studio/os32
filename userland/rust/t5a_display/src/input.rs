use crate::state::{Fixture, Movement};
#[derive(Clone, Copy)]
pub enum Action {
    None,
    Quit,
    Select(Fixture),
    Move(Movement),
}
pub fn key(ch: u8, down: bool) -> Action {
    if !down {
        return Action::None;
    }
    match ch {
        b'1' => Action::Select(Fixture::Normal),
        b'2' => Action::Select(Fixture::Exact),
        b'3' => Action::Select(Fixture::Full),
        b'4' => Action::Select(Fixture::TooWide),
        b'5' => Action::Select(Fixture::Finish),
        b'j' => Action::Move(Movement::Down),
        b'k' => Action::Move(Movement::Up),
        b'g' => Action::Move(Movement::First),
        b'e' => Action::Move(Movement::Last),
        b'q' => Action::Quit,
        _ => Action::None,
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fixed_keys_and_keyup_ignored() {
        assert!(matches!(key(b'1', true), Action::Select(Fixture::Normal)));
        assert!(matches!(key(b'2', true), Action::Select(Fixture::Exact)));
        assert!(matches!(key(b'3', true), Action::Select(Fixture::Full)));
        assert!(matches!(key(b'4', true), Action::Select(Fixture::TooWide)));
        assert!(matches!(key(b'5', true), Action::Select(Fixture::Finish)));
        assert!(matches!(key(b'j', true), Action::Move(Movement::Down)));
        assert!(matches!(key(b'k', true), Action::Move(Movement::Up)));
        assert!(matches!(key(b'g', true), Action::Move(Movement::First)));
        assert!(matches!(key(b'e', true), Action::Move(Movement::Last)));
        assert!(matches!(key(b'q', true), Action::Quit));
        for ch in b"12345jkgeq" {
            assert!(matches!(key(*ch, false), Action::None));
        }
        assert!(matches!(key(0, true), Action::None));
    }
}
