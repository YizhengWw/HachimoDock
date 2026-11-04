use super::*;
use std::cell::RefCell;

fn known_status() -> Value { json!({"ok":true,"state":"buffering","bufferedMs":0}) }

#[test]
fn source_disconnect_reconnects_only_after_confirmed_cleanup() {
    let state=RadioDeviceState::default();let trace=RefCell::new(Vec::new());
    let result=run_radio_attempts(|n| {
        state.request(|| {trace.borrow_mut().push((n,"begin"));Ok(known_status())})?;
        let outcome=if n==0 {Err(RadioPlaybackError::Source("source EOF".into()))} else {Ok(())};
        state.finish(outcome,|| {trace.borrow_mut().push((n,"stop"));Ok(())})
    });
    assert!(result.is_ok());assert_eq!(*trace.borrow(),[(0,"begin"),(0,"stop"),(1,"begin"),(1,"stop")]);
}

#[test]
fn unknown_begin_or_chunk_never_cleans_up_or_restarts() {
    for failed in ["begin","chunk"] {
        let state=RadioDeviceState::default();let trace=RefCell::new(Vec::new());
        let result=run_radio_attempts(|n| {
            let outcome=(|| {
                for phase in ["begin","chunk"] {
                    state.request(|| {trace.borrow_mut().push((n,phase));
                        if phase==failed {Err("indeterminate device result".into())} else {Ok(known_status())}})?;
                }
                Ok(())
            })();
            state.finish(outcome,|| {trace.borrow_mut().push((n,"stop"));Ok(())})
        });
        assert!(matches!(result,Err(RadioPlaybackError::DeviceUnknown(_))));
        let expected=if failed=="begin" {vec![(0,"begin")]} else {vec![(0,"begin"),(0,"chunk")]};
        assert_eq!(*trace.borrow(),expected);
        assert!(state.request(|| {panic!("late stop/new begin must never be sent");#[allow(unreachable_code)] Ok(())}).is_err());
    }
}

#[test]
fn unknown_cleanup_after_source_failure_prevents_reconnect() {
    let state=RadioDeviceState::default();let trace=RefCell::new(Vec::new());
    let result=run_radio_attempts(|n| {
        state.request(|| {trace.borrow_mut().push((n,"begin"));Ok(())})?;
        state.finish(Err::<(),_>(RadioPlaybackError::Source("source EOF".into())),|| {
            trace.borrow_mut().push((n,"stop"));Err("lost stop receipt".into())
        })
    });
    assert!(matches!(result,Err(RadioPlaybackError::DeviceUnknown(_))));
    assert_eq!(*trace.borrow(),[(0,"begin"),(0,"stop")]);
}

#[test]
fn malformed_status_does_not_authorize_cleanup_or_restart() {
    for ack in [json!({}),json!({"ok":"true","state":"buffering","bufferedMs":0}),
        json!({"ok":true,"state":"unknown","bufferedMs":0}),json!({"ok":true,"state":"buffering"}),
        json!({"ok":true,"state":"buffering","bufferedMs":"0"})] {
        let state=RadioDeviceState::default();let trace=RefCell::new(Vec::new());
        let result=run_radio_attempts(|n| {
            let outcome=(|| {
                let value=state.request(|| {trace.borrow_mut().push((n,"begin"));Ok(ack.clone())})?;
                state.status(&value)?;Ok(())
            })();
            state.finish(outcome,|| {panic!("malformed receipt must not authorize stop");#[allow(unreachable_code)] Ok(())})
        });
        assert!(matches!(result,Err(RadioPlaybackError::DeviceUnknown(_))));
        assert_eq!(*trace.borrow(),[(0,"begin")]);
    }
}

#[test]
fn known_terminal_device_state_is_not_a_source_retry() {
    let state=RadioDeviceState::default();let mut calls=0;
    let result=run_radio_attempts(|_| {
        calls+=1;let ack=json!({"ok":true,"state":"interrupted","bufferedMs":0});
        assert_eq!(state.status(&ack)?.1,"interrupted");
        state.finish(Err::<(),_>(RadioPlaybackError::Stopped("device terminal".into())),|| Ok(()))
    });
    assert!(matches!(result,Err(RadioPlaybackError::Stopped(_))));assert_eq!(calls,1);
}

#[test]
fn source_retry_is_bounded_and_local_setup_failure_does_not_retry() {
    let mut calls=0;
    let result=run_radio_attempts(|_| {calls+=1;Err(RadioPlaybackError::Source("source EOF".into()))});
    assert!(matches!(result,Err(RadioPlaybackError::Source(_))));assert_eq!(calls,4);
    calls=0;let result=run_radio_attempts(|_| {calls+=1;Err("decoder unavailable".into())});
    assert!(matches!(result,Err(RadioPlaybackError::Stopped(_))));assert_eq!(calls,1);
}

#[test]
fn device_unknown_outcome_latches_even_if_created_before_cleanup() {
    let state=RadioDeviceState::default();
    let result=state.finish(Err::<(),_>(RadioPlaybackError::DeviceUnknown("unknown".into())),|| {
        panic!("unknown owner cleanup must never submit");#[allow(unreachable_code)] Ok(())
    });
    assert!(matches!(result,Err(RadioPlaybackError::DeviceUnknown(_))));
    assert!(state.ensure_known().is_err());
}

#[test]
fn disconnected_nonempty_session_stop_preserves_owner_and_suppresses_new_begin() {
    let root=tempfile::tempdir().unwrap();
    let ext=crate::component_extension::parse(&std::fs::read(
        std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/city-radio/pc/extension.json")
    ).unwrap()).unwrap();
    let service=create_service("offline-stop".into(),"test".into(),ext,root.path().into(),crate::usb_serial::UsbSerialManager::new());
    *service.session.lock().unwrap()="synthetic-prior-session".into();
    service.pending_auto.store(true,Ordering::SeqCst);
    service.stop("idle","stopped");
    assert_eq!(*service.session.lock().unwrap(),"synthetic-prior-session");
    assert_eq!(service.data.lock().unwrap().state,"error");
    assert!(service.device_state.ensure_known().is_err());
    assert!(!service.pending_auto.load(Ordering::SeqCst));
    assert!(service.device_state.request(|| {panic!("unknown owner must not begin");#[allow(unreachable_code)]Ok(())}).is_err());
}
