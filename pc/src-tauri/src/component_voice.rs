//! Volatile voice of the device's currently displayed pet. No persona text,
//! credentials or per-pet gain is accepted or persisted by this bridge.
use serde::{Deserialize,Serialize};
use std::sync::{Mutex,OnceLock};
use std::time::{Duration, Instant};

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PetVoice {
    board_device_id: String,
    appearance_id: String,
    speaker: String,
    clone_speaker_id: String,
    speed: f32,
}
impl PetVoice {
    fn validate(&self) -> Result<(), String> {
        let identifier = |s: &str, max: usize| !s.is_empty() && s.len() <= max
            && s.bytes().all(|c| c.is_ascii_alphanumeric() || b"_-.".contains(&c));
        if !identifier(&self.board_device_id, 128) || !identifier(&self.appearance_id, 128)
            || !identifier(&self.speaker, 128)
            || !self.clone_speaker_id.is_empty() && !identifier(&self.clone_speaker_id, 128)
            || !self.speed.is_finite() || !(0.5..=2.0).contains(&self.speed)
        {
            return Err("宠物朗读音色配置无效".into());
        }
        Ok(())
    }
}
#[derive(Default)]
struct Current(Option<(PetVoice, Instant)>);
impl Current {
    fn voice(&self, board: &str, now: Instant) -> Option<PetVoice> {
        self.0.as_ref().filter(|(v, at)| v.board_device_id == board
            && now.saturating_duration_since(*at) < Duration::from_secs(30)).map(|(v, _)| v.clone())
    }
}
static CURRENT: Mutex<Current> = Mutex::new(Current(None));
static PREVIOUS:Mutex<Option<PetVoice>>=Mutex::new(None);
static PREPARATION_FILE:OnceLock<std::path::PathBuf>=OnceLock::new();
pub(crate) fn configure_preparation(path:std::path::PathBuf){
    if PREPARATION_FILE.set(path.clone()).is_err(){return;}
    if let Ok(bytes)=std::fs::read(path){if bytes.len()<4096{if let Ok(v)=serde_json::from_slice::<PetVoice>(&bytes){if v.validate().is_ok(){*PREVIOUS.lock().unwrap()=Some(v);}}}}
}
/// Last known voice can prepare disk audio before USB connects; it can NEVER
/// authorize playback. Playback still requires the live CURRENT heartbeat.
pub(crate) fn preparation_config(board:&str)->Result<crate::doubao_tts::DoubaoTtsConfig,String>{
    if !board.is_empty(){return config(board);}
    let voice=PREVIOUS.lock().map_err(|_|"音色缓存不可用")?.clone().ok_or("等待首次选择宠物音色")?;
    crate::voice_chat_settings::tts_config(&voice.speaker,&voice.clone_speaker_id,voice.speed)
}

#[tauri::command]
pub fn component_speech_set_pet_voice(input: Option<PetVoice>) -> Result<(), String> {
    if let Some(v) = &input { v.validate()?; }
    if let Some(v)=&input{
        let mut previous=PREVIOUS.lock().map_err(|_|"音色缓存不可用")?;
        if previous.as_ref()!=Some(v){
            if let Some(path)=PREPARATION_FILE.get(){
                use std::io::Write;
                let _=(||->std::io::Result<()>{let parent=path.parent().unwrap();std::fs::create_dir_all(parent)?;let mut f=tempfile::NamedTempFile::new_in(parent)?;serde_json::to_writer(&mut f,v)?;f.flush()?;f.as_file().sync_all()?;f.persist(path).map_err(|e|e.error)?;Ok(())})();
            }
            *previous=Some(v.clone());
        }
    }
    CURRENT.lock().map_err(|_| "宠物音色暂不可用")?.0 = input.map(|v| (v, Instant::now()));
    Ok(())
}
pub fn ready(board: &str) -> bool {
    CURRENT.lock().ok().and_then(|v| v.voice(board, Instant::now())).is_some()
}
pub fn config(board: &str) -> Result<crate::doubao_tts::DoubaoTtsConfig, String> {
    let voice = CURRENT.lock().map_err(|_| "宠物音色暂不可用")?
        .voice(board, Instant::now()).ok_or("正在等待当前宠物音色，请保持客户端运行")?;
    crate::voice_chat_settings::tts_config(&voice.speaker, &voice.clone_speaker_id, voice.speed)
}

#[cfg(test)]
pub(crate) fn acceptance_persona(root:&std::path::Path,id:&str)->serde_json::Value{
    let path=root.join("custom-appearances").join(id).join("persona-voice.json");
    match std::fs::read(path){
        Ok(raw)=>serde_json::from_slice(&raw).expect("saved voice must be valid"),
        Err(e) if e.kind()==std::io::ErrorKind::NotFound&&id=="builtin-terrier"=>{
            // Same factory fallback used by the frontend when no override exists.
            assert!(include_str!("../../src/lib/persona-voice.js").contains("speaker: \"zh_female_peiqi_uranus_bigtts\""));
            serde_json::json!({"voice":{"speaker":"zh_female_peiqi_uranus_bigtts","clone_speaker_id":"","speed":1.05}})
        },
        Err(e)=>panic!("cannot read selected pet voice: {e}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn pet() -> PetVoice { PetVoice { board_device_id:"board-a".into(), appearance_id:"terrier".into(),
        speaker:"zh_female_vv_uranus_bigtts".into(), clone_speaker_id:String::new(), speed:1.15 } }
    #[test]
    fn current_pet_follows_switch_and_rejects_stale_or_other_board() {
        let now = Instant::now();
        let mut current = Current(Some((pet(), now)));
        assert_eq!(current.voice("board-a", now).unwrap(), pet());
        assert!(current.voice("board-b", now).is_none());
        assert!(current.voice("board-a", now + Duration::from_secs(30)).is_none());
        let mut next = pet(); next.appearance_id="nessie".into(); next.clone_speaker_id="S_test".into();
        current.0=Some((next.clone(), now));
        assert_eq!(current.voice("board-a", now).unwrap(), next);
        let cfg=crate::doubao_tts::DoubaoTtsConfig::new("test", &next.speaker, &next.clone_speaker_id, next.speed, "");
        assert_eq!(cfg.speaker, "S_test"); assert_eq!(cfg.speed, 1.15);
        current.0=None; assert!(current.voice("board-a", now).is_none());
    }
    #[test]
    fn bridge_only_accepts_bounded_voice_fields() {
        assert!(pet().validate().is_ok());
        for speed in [0.0, f32::NAN, 2.1] { let mut v=pet();v.speed=speed;assert!(v.validate().is_err()); }
        let mut v=pet();v.speaker="x".repeat(129);assert!(v.validate().is_err());
        v=pet();v.appearance_id="../other".into();assert!(v.validate().is_err());
        let raw=r#"{"boardDeviceId":"board-a","appearanceId":"pet","speaker":"s","cloneSpeakerId":"","speed":1,"volume":0.3}"#;
        assert!(serde_json::from_str::<PetVoice>(raw).is_err());
    }
}
