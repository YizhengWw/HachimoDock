//! Numeric/stage context only; no port paths, identities or raw protocol data.
use std::io;

#[derive(Clone, Copy)]
pub(super) enum Stage { Open, ReadState, ApplyState, Timeout, Inheritance, Event }
impl Stage {
    fn label(self) -> &'static str {
        match self { Self::Open=>"CreateFileW", Self::ReadState=>"GetCommState", Self::ApplyState=>"SetCommState",
            Self::Timeout=>"SetCommTimeouts",Self::Inheritance=>"SetHandleInformation",Self::Event=>"CreateEventW" }
    }
}
pub(super) fn contextualize(stage: Stage, baud: Option<u32>, error: io::Error) -> io::Error {
    let speed=baud.map_or_else(||"unchanged".to_owned(),|n|n.to_string());
    let code=error.raw_os_error().map_or_else(||"unavailable".to_owned(),|n|n.to_string());
    io::Error::new(error.kind(),format!("Windows serial {} failed (baud={speed}, osCode={code}): {error}",stage.label()))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn win32_stage_and_original_error_code_survive_without_private_context() {
        for (stage,name) in [(Stage::Open,"CreateFileW"),(Stage::ReadState,"GetCommState"),(Stage::ApplyState,"SetCommState"),
            (Stage::Timeout,"SetCommTimeouts"),(Stage::Inheritance,"SetHandleInformation"),(Stage::Event,"CreateEventW")] {
            let original=io::Error::from_raw_os_error(1);let kind=original.kind();
            let error=contextualize(stage,Some(4_000_000),original);
            assert_eq!(error.kind(),kind);let text=error.to_string();
            assert!(text.contains(name)&&text.contains("baud=4000000")&&text.contains("osCode=1"));
            assert!(!text.contains("COM6")&&!text.contains("/Users/")&&!text.contains("boardDeviceId"));
        }
        let error=contextualize(Stage::Timeout,None,io::Error::from_raw_os_error(5));
        assert!(error.to_string().contains("baud=unchanged, osCode=5"));
    }
}
