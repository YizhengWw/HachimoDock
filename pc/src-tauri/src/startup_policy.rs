//! Bounded native startup policy: no IO, allocation, threads, clocks or Tauri.
pub const TIMEOUT_MS: u64 = 25_000;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Choice { Reload, Exit, Dismiss }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Effect { Reload, Exit, Ignore }
#[derive(Debug)]
pub struct Policy {
    generation: u64,
    started_ms: u64,
    ready: bool,
    notified: bool,
    dialog: Option<u64>,
    closed: bool,
}
impl Policy {
    pub fn new(now: u64) -> Self { Self { generation:1, started_ms:now, ready:false, notified:false, dialog:None, closed:false } }
    pub fn generation(&self) -> u64 { self.generation }
    pub fn closed(&self) -> bool { self.closed }
    pub fn navigate(&mut self, now: u64) -> bool {
        if self.closed { return false; }
        let Some(next)=self.generation.checked_add(1) else { self.close();return false; };
        self.generation=next;self.started_ms=now;self.ready=false;self.notified=false;
        // An already-open older dialog still occupies the only dialog slot.
        true
    }
    pub fn ready(&mut self, generation: u64) -> bool {
        if self.closed || generation!=self.generation { return false; }
        self.ready=true;true
    }
    pub fn poll(&mut self, now: u64) -> Option<u64> {
        if self.closed || self.ready || self.notified || self.dialog.is_some()
            || now.saturating_sub(self.started_ms)<TIMEOUT_MS { return None; }
        self.notified=true;self.dialog=Some(self.generation);Some(self.generation)
    }
    pub fn can_present(&self, generation: u64) -> bool {
        !self.closed && !self.ready && self.generation==generation && self.dialog==Some(generation)
    }
    pub fn choose(&mut self, generation: u64, choice: Choice, now: u64) -> Effect {
        if self.dialog!=Some(generation) { return Effect::Ignore; }
        self.dialog=None;
        if self.closed || self.ready || generation!=self.generation { return Effect::Ignore; }
        match choice {
            Choice::Reload => if self.navigate(now) { Effect::Reload } else { Effect::Ignore },
            Choice::Exit => Effect::Exit,
            Choice::Dismiss => Effect::Ignore,
        }
    }
    pub fn close(&mut self) { self.closed=true;self.dialog=None; }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn ready_suppresses_timeout_and_duplicate_ready_is_idempotent() {
        let mut p=Policy::new(0);assert!(p.ready(1));assert!(p.ready(1));assert_eq!(p.poll(TIMEOUT_MS*5),None);
    }
    #[test] fn timeout_notifies_once_and_dismiss_does_not_repeat() {
        let mut p=Policy::new(5);assert_eq!(p.poll(TIMEOUT_MS+4),None);assert_eq!(p.poll(TIMEOUT_MS+5),Some(1));
        assert_eq!(p.poll(TIMEOUT_MS*2),None);assert_eq!(p.choose(1,Choice::Dismiss,TIMEOUT_MS*2),Effect::Ignore);
        assert_eq!(p.poll(TIMEOUT_MS*3),None);
    }
    #[test] fn old_navigation_ready_and_queued_notice_are_rejected() {
        let mut p=Policy::new(0);assert_eq!(p.poll(TIMEOUT_MS),Some(1));p.navigate(TIMEOUT_MS);
        assert!(!p.ready(1));assert!(!p.can_present(1));assert_eq!(p.choose(1,Choice::Reload,TIMEOUT_MS+1),Effect::Ignore);
        assert_eq!(p.poll(TIMEOUT_MS*2),Some(2));
    }
    #[test] fn late_ready_prevents_already_open_dialog_action() {
        for choice in [Choice::Reload,Choice::Exit] {
            let mut p=Policy::new(0);p.poll(TIMEOUT_MS);assert!(p.ready(1));
            assert_eq!(p.choose(1,choice,TIMEOUT_MS+1),Effect::Ignore);
        }
    }
    #[test] fn user_reload_advances_generation_and_never_repeats_old_action() {
        let mut p=Policy::new(0);p.poll(TIMEOUT_MS);
        assert_eq!(p.choose(1,Choice::Reload,TIMEOUT_MS),Effect::Reload);assert_eq!(p.generation(),2);
        assert_eq!(p.choose(1,Choice::Reload,TIMEOUT_MS),Effect::Ignore);assert_eq!(p.poll(TIMEOUT_MS+1),None);
    }
    #[test] fn an_old_open_dialog_cannot_accumulate_with_new_navigation() {
        let mut p=Policy::new(0);p.poll(TIMEOUT_MS);
        for i in 1..100 {p.navigate(i*TIMEOUT_MS);assert_eq!(p.poll((i+1)*TIMEOUT_MS),None);}
        assert_eq!(p.choose(1,Choice::Exit,100*TIMEOUT_MS),Effect::Ignore);
        assert_eq!(p.poll(101*TIMEOUT_MS),Some(100));
    }
    #[test] fn explicit_exit_is_only_an_effect_of_current_user_choice() {
        let mut p=Policy::new(0);p.poll(TIMEOUT_MS);assert_eq!(p.choose(1,Choice::Exit,TIMEOUT_MS),Effect::Exit);
    }
    #[test] fn shutdown_blocks_ready_timeout_and_old_callbacks() {
        let mut p=Policy::new(0);p.poll(TIMEOUT_MS);p.close();
        assert!(p.closed());assert!(!p.ready(1));assert!(!p.navigate(1));assert_eq!(p.poll(u64::MAX),None);
        assert_eq!(p.choose(1,Choice::Reload,u64::MAX),Effect::Ignore);
    }
}
