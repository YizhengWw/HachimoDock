//! One recording owns only its appended suffix, never the pre-existing draft.
pub(crate) struct VoiceDraft {
    pub base: String,
    pub last: String,
    insertion: Option<usize>,
}

impl VoiceDraft {
    pub fn new(base: String) -> Self { Self { last: base.clone(), base, insertion: None } }
    pub fn at_utf16_caret(base: String, location: usize) -> Result<Self, String> {
        let mut units = 0;
        let mut offset = None;
        for (byte, ch) in base.char_indices() {
            if units == location { offset = Some(byte); break; }
            units += ch.len_utf16();
        }
        if offset.is_none() && units == location { offset = Some(base.len()); }
        let offset = offset.ok_or("无法确认语音插入光标位置，尚未写入文字")?;
        Ok(Self { last:base.clone(), base, insertion:Some(offset) })
    }
    pub fn caret_after(&self, transcript: &str) -> Option<usize> {
        self.insertion.map(|offset| self.base[..offset].encode_utf16().count() + transcript.trim().encode_utf16().count())
    }
    pub fn desired(&self, transcript: &str) -> String {
        let text = transcript.trim();
        if let Some(offset) = self.insertion {
            return format!("{}{}{}", &self.base[..offset], text, &self.base[offset..]);
        }
        let separator = if self.base.is_empty() || text.is_empty() || self.base.ends_with(char::is_whitespace) { "" } else { " " };
        format!("{}{separator}{text}", self.base)
    }
    pub fn can_update(&self, current: &str, desired: &str) -> bool {
        current == self.last || current == desired
    }
    pub fn rollback<'a>(&'a self, current: &str) -> Option<&'a str> {
        (current == self.last && self.last != self.base).then_some(self.base.as_str())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn caret_insertion_preserves_lines_suffix_and_surrogate_boundaries() {
        let mut draft = VoiceDraft::at_utf16_caret("前🙂后\n下一行\n".into(), 3).unwrap();
        assert_eq!(draft.desired("第一句"), "前🙂第一句后\n下一行\n");
        assert_eq!(draft.caret_after("第一句"), Some(6));
        draft.last = draft.desired("第一");
        assert_eq!(draft.desired("第一句"), "前🙂第一句后\n下一行\n");
        assert!(!draft.can_update("手动改动", &draft.desired("第一句")));
        assert!(VoiceDraft::at_utf16_caret("🙂".into(), 1).is_err());
        assert!(VoiceDraft::at_utf16_caret("a".into(), 2).is_err());
        let empty = VoiceDraft::at_utf16_caret("\n".into(), 0).unwrap();
        assert_eq!(empty.desired("测试"), "测试\n");
        let second = VoiceDraft::at_utf16_caret(draft.desired("第一句"), 6).unwrap();
        assert_eq!(second.desired("第二句"), "前🙂第一句第二句后\n下一行\n");
    }
    #[test]
    fn multiple_recordings_and_partial_revisions_preserve_previous_draft() {
        let mut first = VoiceDraft::new("用户原文\n  保留缩进".into());
        first.last = first.desired("第一");
        assert_eq!(first.desired("第一句话。"), "用户原文\n  保留缩进 第一句话。");
        first.last = first.desired("第一句话。");
        let mut second = VoiceDraft::new(first.last.clone());
        second.last = second.desired("再补");
        let final_value = second.desired("再补充一句。");
        assert_eq!(final_value, "用户原文\n  保留缩进 第一句话。 再补充一句。");
        assert!(second.can_update(&second.last, &final_value));
        assert!(second.can_update(&final_value, &final_value));
        assert!(!second.can_update("手动修改", &final_value));
        assert_eq!(second.rollback(&second.last), Some(first.last.as_str()));
        assert_eq!(second.rollback("手动修改"), None);
    }
    #[test]
    fn empty_speech_whitespace_unicode_and_repeated_text_are_lossless() {
        for base in ["", "  ", "多行\n", "代码\n    ", "🙂 hello"] {
            let draft = VoiceDraft::new(base.into());
            assert_eq!(draft.desired("  "), base);
            assert_eq!(draft.rollback(base), None);
        }
        assert_eq!(VoiceDraft::new("hello ".into()).desired(" world "), "hello world");
        assert_eq!(VoiceDraft::new("再说一次".into()).desired("再说一次"), "再说一次 再说一次");
    }
}
