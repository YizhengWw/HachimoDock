//! Pure bounded SendInput prefix accounting. Never repeats an action.
use std::collections::BTreeSet;
pub type Key = u16;
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Event { pub key: Key, pub up: bool }
pub trait Transport {
    fn held(&mut self, key: Key) -> bool;
    /// Exact inserted prefix count; this is NOT target-application completion.
    fn insert(&mut self, events: &[Event]) -> usize;
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Failure {
    Invalid,
    Held,
    Unknown { accepted: usize, requested: usize, release_needed: usize, release_acked: usize },
}
pub fn dispatch(events: &[Event], io: &mut impl Transport) -> Result<(), Failure> {
    if events.len() > 8 { return Err(Failure::Invalid); }
    if events.is_empty() { return Ok(()); }
    let mut pressed = BTreeSet::new();
    let mut keys = BTreeSet::new();
    for event in events {
        if event.key == 0 || event.key > 255 { return Err(Failure::Invalid); }
        keys.insert(event.key);
        if event.up { if !pressed.remove(&event.key) { return Err(Failure::Invalid); } }
        else if !pressed.insert(event.key) { return Err(Failure::Invalid); }
    }
    if !pressed.is_empty() { return Err(Failure::Invalid); }
    // Never synthesize a key-up for an already-held user key. Also reject
    // unrelated held modifiers rather than accidentally changing the chord.
    for vk in [0x10, 0x11, 0x12, 0x5b, 0x5c] { keys.insert(vk); }
    for key in keys { if io.held(key) { return Err(Failure::Held); } }
    let accepted = io.insert(events);
    if accepted == events.len() { return Ok(()); }
    let mut pending = Vec::new();
    if accepted <= events.len() {
        for event in &events[..accepted] {
            if event.up { if let Some(i) = pending.iter().position(|key| *key == event.key) { pending.remove(i); } }
            else { pending.push(event.key); }
        }
    }
    let needed = pending.len();
    let mut acked = 0;
    for key in pending.into_iter().rev() {
        // One release-only attempt per outstanding key. Continue other key
        // releases even if one is rejected. Never repeat key-down/text/click.
        if io.insert(&[Event { key, up: true }]) == 1 { acked += 1; }
    }
    Err(Failure::Unknown { accepted, requested: events.len(), release_needed: needed, release_acked: acked })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    struct Memory { keys:BTreeSet<Key>, replies:VecDeque<usize>, calls:Vec<Vec<Event>> }
    impl Memory { fn new(replies:Vec<usize>)->Self{Self{keys:BTreeSet::new(),replies:replies.into(),calls:Vec::new()}} }
    impl Transport for Memory {
        fn held(&mut self,key:Key)->bool { self.keys.contains(&key) }
        fn insert(&mut self,events:&[Event])->usize {
            self.calls.push(events.to_vec());let accepted=self.replies.pop_front().unwrap_or(events.len());
            for e in events.iter().take(accepted) {if e.up{self.keys.remove(&e.key);}else{self.keys.insert(e.key);}}
            accepted
        }
    }
    fn key(v:u16,up:bool)->Event{Event{key:v,up}}
    fn batches()->Vec<Vec<Event>>{
        let mut result=vec![vec![key(0x11,false),key(0x43,false),key(0x43,true),key(0x11,true)]];
        let codes=[0x11,0x10,0x12,0x41];
        result.push(codes.iter().map(|v|key(*v,false)).chain(codes.iter().rev().map(|v|key(*v,true))).collect());result
    }
    #[test]fn every_successful_cleanup_of_every_partial_prefix_releases_only_owned_keys(){
        for events in batches(){for cut in 0..events.len(){let mut io=Memory::new(vec![cut]);let result=dispatch(&events,&mut io);assert!(matches!(result,Err(Failure::Unknown{..})));assert!(io.keys.is_empty(),"cut={cut}");assert_eq!(io.calls[0],events);assert!(io.calls.iter().skip(1).flatten().all(|e|e.up));assert!(io.calls.iter().skip(1).all(|c|c.len()==1));}}
    }
    #[test]fn rejected_release_is_unknown_and_never_replays_or_hides_remaining_down_state(){
        let events=batches().remove(0);let mut io=Memory::new(vec![2,0,1]);
        assert_eq!(dispatch(&events,&mut io),Err(Failure::Unknown{accepted:2,requested:4,release_needed:2,release_acked:1}));
        assert_eq!(io.keys,BTreeSet::from([0x43]));assert_eq!(io.calls.len(),3);assert!(io.calls[1..].iter().flatten().all(|e|e.up));
    }
    #[test]fn preexisting_user_keys_and_modifiers_are_never_released(){
        let events=batches().remove(0);
        for held in [0x11,0x43,0x10,0x5c]{let mut io=Memory::new(vec![]);io.keys.insert(held);assert_eq!(dispatch(&events,&mut io),Err(Failure::Held));assert!(io.calls.is_empty());assert_eq!(io.keys,BTreeSet::from([held]));}
    }
    #[test]fn unrelated_user_key_is_never_part_of_cleanup(){let events=batches().remove(0);let mut io=Memory::new(vec![2]);io.keys.insert(0x58);assert!(dispatch(&events,&mut io).is_err());assert_eq!(io.keys,BTreeSet::from([0x58]));assert!(io.calls[1..].iter().flatten().all(|e|e.key!=0x58));}
    #[test]fn full_insertion_is_one_batch_and_not_application_completion(){for events in batches(){let mut io=Memory::new(vec![]);assert_eq!(dispatch(&events,&mut io),Ok(()));assert_eq!(io.calls.len(),1);assert!(io.keys.is_empty());}}
    #[test]fn malformed_batches_fail_before_any_input(){for events in [vec![key(0x11,false)],vec![key(0x11,true)],vec![key(0x11,false),key(0x11,false),key(0x11,true)],vec![key(0,false),key(0,true)]]{let mut io=Memory::new(vec![]);assert_eq!(dispatch(&events,&mut io),Err(Failure::Invalid));assert!(io.calls.is_empty());}}
    #[test]fn legacy_batch_check_leaves_down_prefix_in_memory(){let events=batches().remove(0);let mut io=Memory::new(vec![2]);let sent=io.insert(&events);assert_ne!(sent,events.len());assert_eq!(io.keys,BTreeSet::from([0x11,0x43]));}
}
