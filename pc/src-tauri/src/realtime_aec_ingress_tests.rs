// Actual production ingress tests, included inside realtime_chat::tests.
mod aec_ingress_metadata_tests {
use super::*;

fn session() -> (Session, mpsc::Receiver<Input>) {
    let (s,rx)=routing_session();
    s.capture_armed.store(true,Ordering::SeqCst);
    s.capture_ready.store(true,Ordering::SeqCst);
    s.status.lock().unwrap().state="listening".into();
    (s,rx)
}

fn frame() -> Value {json!({"boardDeviceId":"board","conversationSessionId":"rtc-new",
    "aec":true,"vadSpeech":false,"data":BASE64.encode(vec![0;FRAME_BYTES])})}

#[test] fn processed_current_audio_is_the_only_duplex_ingress_into_real_pcm_channel() {
    for speech in [false,true] {
        let (s,mut rx)=session();let mut p=frame();p["vadSpeech"]=json!(speech);
        assert!(route_session_audio(&s,"audio/chunk",&p));
        assert!(matches!(rx.try_recv(),Ok(Input::Pcm(data,Some(hint))) if data==vec![0;FRAME_BYTES] && hint==speech));
        assert!(!s.cancelled.load(Ordering::SeqCst));
    }
}
#[test] fn unprocessed_audio_never_reaches_asr_channel_and_retains_original_stop_reason() {
    let (s,mut rx)=session();let mut p=frame();p["aec"]=json!(false);
    assert!(route_session_audio(&s,"audio/chunk",&p));
    assert!(rx.try_recv().is_err());assert!(s.cancelled.load(Ordering::SeqCst));
    let status=s.status.lock().unwrap();assert_eq!(status.reason,"aec_unavailable");assert!(status.error.contains("具体原因未确定"));
}
#[test] fn absent_and_non_boolean_aec_or_vad_fail_closed_without_queueing_pcm() {
    for (key,value) in [("aec",None),("aec",Some(json!("true"))),("aec",Some(Value::Null)),
        ("vadSpeech",None),("vadSpeech",Some(json!(0))),("vadSpeech",Some(json!({"transcript":"PRIVATE"})))] {
        let (s,mut rx)=session();let mut p=frame();
        if let Some(value)=value {p[key]=value;}else{p.as_object_mut().unwrap().remove(key);}
        assert!(route_session_audio(&s,"audio/chunk",&p));assert!(s.cancelled.load(Ordering::SeqCst));assert!(rx.try_recv().is_err());
        assert!(s.status.lock().unwrap().error.contains("语音数据"));
    }
}
#[test] fn preparing_status_waits_without_admitting_raw_audio_or_reporting_dsp_failure() {
    let (s,mut rx)=session();s.capture_ready.store(false,Ordering::SeqCst);
    assert!(route_session_audio(&s,"audio/status",&json!({"conversationSessionId":"rtc-new","conversation":true,"audioAecPreparing":true,"audioAec":false})));
    assert!(!s.capture_ready.load(Ordering::SeqCst));assert!(!s.cancelled.load(Ordering::SeqCst));assert!(rx.try_recv().is_err());
}
#[test] fn status_failure_does_not_invent_a_dsp_root_cause() {
    let (s,_)=session();assert!(route_session_audio(&s,"audio/status",&json!({"conversationSessionId":"rtc-new","conversation":true,"audioAec":false,"aecReason":"none"})));
    assert!(s.cancelled.load(Ordering::SeqCst));let state=s.status.lock().unwrap();assert!(state.error.contains("具体原因未确定"));assert!(!state.error.contains("初始化失败"));
}
#[test] fn known_device_reported_faults_keep_specific_human_explanation_but_not_raw_device_text() {
    for (reason,expected) in [("init_failed","初始化失败"),("stream_reset_failed","初始化失败"),("reference_overflow","参考缓冲溢出"),("processed_queue_overflow","处理缓冲溢出"),("dsp_too_slow","持续超时")] {
        let(s,_)=session();assert!(route_session_audio(&s,"audio/status",&json!({"conversation":true,"audioAec":false,"aecReason":reason})));
        assert!(s.status.lock().unwrap().error.contains(expected));assert!(s.cancelled.load(Ordering::SeqCst));
    }
}
#[test] fn old_timeline_and_other_board_are_not_relabelled_as_current_failure_or_queued() {
    for timeline in ["","rtc-old"] {let(s,mut rx)=session();let mut p=frame();p["conversationSessionId"]=json!(timeline);p["aec"]=json!(false);
        assert!(route_session_audio(&s,"audio/chunk",&p));assert!(!s.cancelled.load(Ordering::SeqCst));assert!(rx.try_recv().is_err());}
    let(s,mut rx)=session();let mut p=frame();p["boardDeviceId"]=json!("other");p["aec"]=json!(false);
    assert!(!route_session_audio(&s,"audio/chunk",&p));assert!(!s.cancelled.load(Ordering::SeqCst));assert!(rx.try_recv().is_err());
}
#[test] fn legacy_pre_ready_exception_and_post_ready_rejection_are_unchanged() {
    let(s,mut rx)=session();s.capture_ready.store(false,Ordering::SeqCst);
    assert!(route_session_audio(&s,"audio/chunk",&json!({"aec":false})));assert!(!s.cancelled.load(Ordering::SeqCst));
    route_session_audio(&s,"audio/status",&json!({"conversation":true,"audioAec":true}));assert!(s.capture_ready.load(Ordering::SeqCst));
    route_session_audio(&s,"audio/chunk",&json!({"aec":true}));assert!(s.cancelled.load(Ordering::SeqCst));assert!(rx.try_recv().is_err());
}
#[test] fn before_arming_and_after_cancellation_no_pcm_is_forwarded() {
    let(s,mut rx)=session();s.capture_armed.store(false,Ordering::SeqCst);route_session_audio(&s,"audio/chunk",&frame());assert!(rx.try_recv().is_err());
    s.capture_armed.store(true,Ordering::SeqCst);s.cancelled.store(true,Ordering::SeqCst);route_session_audio(&s,"audio/chunk",&frame());assert!(rx.try_recv().is_err());
}
#[test] fn invalid_pcm_size_remains_dropped_and_channel_overflow_remains_fail_closed() {
    // A malformed current, processed frame now ends this round explicitly.
    // Later valid PCM cannot revive it or replace its first failure cause.
    {
        let(s,mut rx)=session();let mut p=frame();p["sessionId"]=json!("capture-current");
        p["data"]=json!(BASE64.encode(vec![0;FRAME_BYTES-1]));
        assert!(route_session_audio(&s,"audio/chunk",&p));
        assert!(rx.try_recv().is_err());assert!(s.cancelled.load(Ordering::SeqCst));
        assert_eq!(s.status.lock().unwrap().reason,"capture_frame_invalid");
        assert!(route_session_audio(&s,"audio/chunk",&frame()));
        assert!(rx.try_recv().is_err());
        assert_eq!(s.status.lock().unwrap().reason,"capture_frame_invalid");
    }
    // Overflow needs a fresh round; the invalid-frame round is terminal.
    let(s,mut rx)=session();let capacity=rx.capacity();let mut p=frame();p["sessionId"]=json!("capture-current");
    for _ in 0..capacity {assert!(route_session_audio(&s,"audio/chunk",&p));}
    assert_eq!(rx.len(),capacity);assert!(!s.cancelled.load(Ordering::SeqCst));
    assert!(route_session_audio(&s,"audio/chunk",&p));
    assert!(s.cancelled.load(Ordering::SeqCst));assert_eq!(s.status.lock().unwrap().reason,"capture_overflow");
    assert_eq!(rx.len(),capacity);
    for _ in 0..capacity {
        assert!(matches!(rx.try_recv(),Ok(Input::Pcm(data,Some(false))) if data==vec![0;FRAME_BYTES]));
    }
    assert!(rx.try_recv().is_err());
}

}
