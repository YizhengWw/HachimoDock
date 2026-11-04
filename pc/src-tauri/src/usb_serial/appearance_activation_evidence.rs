// Metadata-only activation observation. It never authorizes a retry or clears a journal.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Stage { Admission, Write, Ack, Response }
impl Stage {pub(super) fn label(self)-> &'static str {match self {Self::Admission=>"admission",Self::Write=>"write",Self::Ack=>"ack",Self::Response=>"response"}}}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Kind { AdmissionRefused, WriteFailed, PostWriteFailed, AckTimeout, AckChannelClosed, Cancelled, AckReadFailed, MalformedResponse, DeviceRejected, None }
impl Kind {pub(super) fn label(self)-> &'static str {match self {
 Self::AdmissionRefused=>"admission_failed",Self::WriteFailed=>"write_failed",Self::PostWriteFailed=>"post_write_failed",Self::AckTimeout=>"ack_timeout",Self::AckChannelClosed=>"ack_channel_closed",Self::Cancelled=>"cancelled",Self::AckReadFailed=>"ack_read_failed",Self::MalformedResponse=>"malformed_response",Self::DeviceRejected=>"device_rejected",Self::None=>"none"}}}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Ack { NotObserved, Positive, ExactNegative, Malformed }
impl Ack {pub(super) fn label(self)-> &'static str {match self {Self::NotObserved=>"not_observed",Self::Positive=>"positive",Self::ExactNegative=>"exact_negative",Self::Malformed=>"malformed"}}}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct Evidence {pub(super) stage:Stage,pub(super) error_kind:Kind,pub(super) write_started:bool,pub(super) write_completed:bool,pub(super) ack:Ack}
impl Default for Evidence {fn default()->Self {Self{stage:Stage::Admission,error_kind:Kind::AdmissionRefused,write_started:false,write_completed:false,ack:Ack::NotObserved}}}
impl Evidence {
 pub(super) fn write_started(&mut self){self.stage=Stage::Write;self.error_kind=Kind::WriteFailed;self.write_started=true;}
 pub(super) fn write_completed(&mut self){self.error_kind=Kind::PostWriteFailed;self.write_completed=true;}
 pub(super) fn ack_wait_started(&mut self){self.stage=Stage::Ack;self.error_kind=Kind::AckReadFailed;}
 pub(super) fn fail(&mut self,kind:Kind){self.error_kind=kind;}
 pub(super) fn response(&mut self,ok:Option<bool>){self.stage=Stage::Response;let (ack,kind)=match ok {Some(true)=>(Ack::Positive,Kind::None),Some(false)=>(Ack::ExactNegative,Kind::DeviceRejected),None=>(Ack::Malformed,Kind::MalformedResponse)};self.ack=ack;self.error_kind=kind;}
}

#[cfg(test)]
mod tests {
 use super::*;
 #[test] fn admission_has_no_write_proof(){let e=Evidence::default();assert_eq!(e.stage,Stage::Admission);assert!(!e.write_started);assert!(!e.write_completed);assert_eq!(e.ack,Ack::NotObserved);}
 #[test] fn partial_or_failed_write_is_not_complete(){let mut e=Evidence::default();e.write_started();assert!(e.write_started);assert!(!e.write_completed);assert_eq!(e.stage,Stage::Write);assert_eq!(e.error_kind,Kind::WriteFailed);}
 #[test] fn completed_local_write_is_not_device_ack(){let mut e=Evidence::default();e.write_started();e.write_completed();e.ack_wait_started();assert_eq!(e.stage,Stage::Ack);assert_eq!(e.ack,Ack::NotObserved);}
 #[test] fn exact_negative_distinct_from_missing_or_non_bool(){for ok in [None,Some(false),Some(true)]{let mut e=Evidence::default();e.write_started();e.write_completed();e.response(ok);assert_eq!(e.stage,Stage::Response);match ok{None=>assert_eq!(e.ack,Ack::Malformed),Some(false)=>assert_eq!(e.error_kind,Kind::DeviceRejected),Some(true)=>assert_eq!(e.ack,Ack::Positive)}}}
 #[test] fn timeout_is_not_negative_response(){let mut e=Evidence::default();e.write_started();e.write_completed();e.ack_wait_started();e.fail(Kind::AckTimeout);assert_eq!(e.stage,Stage::Ack);assert_eq!(e.ack,Ack::NotObserved);assert_eq!(e.error_kind,Kind::AckTimeout);}
}
