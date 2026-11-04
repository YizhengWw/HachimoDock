//! Bounded explicit-voice wake ordering. No native, network or UI side effects.

pub(super) fn unique_existing_pid(rows: &[(i32, bool, bool)]) -> Result<Option<i32>, String> {
    let live = rows.iter().filter(|(_, _, terminated)| !terminated).collect::<Vec<_>>();
    match live.as_slice() {
        [] => Ok(None),
        [row] if row.0 > 0 && row.1 => Ok(Some(row.0)),
        _ => Err("WorkBuddy 运行实例身份不唯一或不匹配，未选择其它实例".into()),
    }
}

/// Read-only readiness for one independently pinned AppKit application.
/// Missing AX focused-app metadata does not overrule exact active PID plus
/// actual windows. An explicit foreign AX focus is never accepted. This does
/// not authorize editor writes; their own identity and focus checks still run.
pub(super) fn windows_ready(
    expected_pid: i32,
    application_active: bool,
    focused_pid: Option<i32>,
    window_count: usize,
) -> bool {
    expected_pid > 0
        && application_active
        && window_count > 0
        && focused_pid.map_or(true, |pid| pid == expected_pid)
}

pub(super) fn prepare_existing_once(
    pid: Option<i32>,
    activate: impl FnOnce(i32) -> Result<(), String>,
    verify_windows: impl FnOnce(i32) -> Result<bool, String>,
    cache_ready: impl FnOnce(i32, bool),
) -> Result<(), String> {
    let Some(pid) = pid else { return Ok(()); };
    if pid <= 0 { return Err("WorkBuddy 运行实例无效，未唤起".into()); }
    activate(pid)?;
    let primed = verify_windows(pid)?;
    cache_ready(pid, primed);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};

    #[test]
    fn exact_single_live_bundle_instance_is_required() {
        assert_eq!(unique_existing_pid(&[]).unwrap(), None);
        assert_eq!(unique_existing_pid(&[(7,true,true)]).unwrap(), None);
        assert_eq!(unique_existing_pid(&[(17,true,false)]).unwrap(), Some(17));
        for rows in [&[(17,false,false)][..], &[(0,true,false)][..],
                     &[(17,true,false),(18,true,false)][..]] {
            assert!(unique_existing_pid(rows).is_err());
        }
    }

    #[test]
    fn activate_precedes_real_window_proof_and_success_cache() {
        let trace=RefCell::new(Vec::new());
        prepare_existing_once(Some(17),
            |pid| { assert_eq!(pid,17); trace.borrow_mut().push("activate-existing-once");Ok(()) },
            |pid| { assert_eq!(pid,17); trace.borrow_mut().push("real-window-ready");Ok(true) },
            |pid,primed| { assert_eq!(pid,17);assert!(primed);trace.borrow_mut().push("cache-ready"); },
        ).unwrap();
        assert_eq!(*trace.borrow(),["activate-existing-once","real-window-ready","cache-ready"]);
    }

    #[test]
    fn zero_windows_or_activation_failure_never_caches_success() {
        let activations=Cell::new(0);
        let r=prepare_existing_once(Some(17),
            |_| {activations.set(activations.get()+1);Ok(())},
            |_| Err("AX zero windows".into()),
            |_,_| panic!("no real windows: no cached readiness"));
        assert!(r.is_err());assert_eq!(activations.get(),1);
        assert!(prepare_existing_once(Some(17), |_|Err("activation unknown".into()),
            |_|panic!("no read after denied activation"), |_,_|panic!("no cache")).is_err());
    }

    #[test]
    fn no_instance_or_invalid_pin_never_activates_or_creates_one() {
        assert!(prepare_existing_once(None, |_|panic!("must not launch"),
            |_|panic!("must not query"), |_,_|panic!("no cache")).is_ok());
        assert!(prepare_existing_once(Some(0), |_|panic!("invalid pid"),
            |_|panic!("must not query"), |_,_|panic!("no cache")).is_err());
    }

    #[test]
    fn native_wake_is_only_reached_from_explicit_voice_begin() {
        let source = include_str!("codex_composer_macos.rs");
        let observer = source.split("fn agent_applications(").nth(1).unwrap()
            .split("fn is_agent_primary_window_title(").next().unwrap();
        assert!(!observer.contains("prepare_existing_workbuddy_for_voice_begin"));
        assert!(!observer.contains("activateWithOptions"));
        let current = source.split("pub(super) fn begin_current_voice(").nth(1).unwrap()
            .split("fn task_window_identity(").next().unwrap();
        assert!(current.find("prepare_existing_workbuddy_for_voice_begin(agent)?").unwrap()
            < current.find("find_current_visible_target(agent)?").unwrap());
        let bound = source.split("pub(super) fn begin_voice(").nth(1).unwrap()
            .split("pub(super) fn begin_current_voice(").next().unwrap();
        assert!(bound.find("prepare_existing_workbuddy_for_voice_begin(agent)?").unwrap()
            < bound.find("focus_session(agent,").unwrap());
        let update = source.split("pub(super) fn update_voice(").nth(1).unwrap()
            .split("fn send_button_label_score(").next().unwrap();
        assert!(!update.contains("prepare_existing_workbuddy_for_voice_begin"));
        assert_eq!(source.matches("prepare_existing_workbuddy_for_voice_begin(").count(), 3);
    }

    #[test]
    fn stale_owner_or_focus_change_stops_once_without_cache_or_retry() {
        let attempts=Cell::new(0);
        assert!(prepare_existing_once(Some(17),
            |_| {attempts.set(attempts.get()+1);Ok(())},
            |_| Err("pinned process/focus changed".into()),
            |_,_|panic!("stale process must not cache")).is_err());
        assert_eq!(attempts.get(),1);
    }
    #[test]
    fn unknown_ax_focus_accepts_only_active_pinned_app_with_real_windows() {
        assert!(windows_ready(17, true, None, 1));
        assert!(windows_ready(17, true, Some(17), 2));
    }

    #[test]
    fn inactive_windowless_or_invalid_pins_are_not_ready() {
        for focused in [None, Some(17)] {
            assert!(!windows_ready(17, false, focused, 1));
            assert!(!windows_ready(17, true, focused, 0));
        }
        for pid in [0, -1] {
            assert!(!windows_ready(pid, true, None, 1));
            assert!(!windows_ready(pid, true, Some(pid), 1));
        }
    }

    #[test]
    fn explicit_foreign_or_invalid_ax_focus_is_never_ignored() {
        for focused in [18, 0, -1] {
            assert!(!windows_ready(17, true, Some(focused), 1));
        }
    }

    #[test]
    fn readiness_is_not_latched_after_success_or_failure() {
        assert!(!windows_ready(17, true, Some(18), 1));
        assert!(windows_ready(17, true, None, 1));
        assert!(!windows_ready(17, false, None, 1));
        assert!(windows_ready(17, true, Some(17), 1));
        assert!(!windows_ready(17, true, Some(18), 1));
    }

    #[test]
    fn denied_readiness_never_reaches_prepare_cache_and_does_not_poison_next_call() {
        let cached=Cell::new(0);
        for focused in [Some(18), None] {
            let result=prepare_existing_once(Some(17), |_| Ok(()), |pid| {
                if windows_ready(pid, true, focused, 1) { Ok(true) }
                else { Err("readiness not proved".into()) }
            }, |_,ready| { assert!(ready); cached.set(cached.get()+1); });
            assert_eq!(result.is_ok(), focused.is_none());
        }
        assert_eq!(cached.get(), 1);
    }

}
