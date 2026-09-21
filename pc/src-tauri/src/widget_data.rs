//! Bounded, read-only data snapshots shared by PC providers and device widgets.
//! Providers own networking; packages only subscribe to a named source.
use serde::{Deserialize, Serialize};

pub const MAX_ROWS: usize = 20;
pub const PROTOCOL: &str = "p4-data-list-v1";
#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct DataRow {
    pub id: String,
    pub label: String,
    pub value: String,
    pub detail: String,
    pub meta: String,
    pub tone: i8,
}
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DataSnapshot {
    pub schema: u8,
    pub date: String,
    pub source: String,
    pub ttl_ms: u32,
    pub status: String,
    pub message: String,
    pub rows: Vec<DataRow>,
}
pub fn bounded_text(value: &str, limit: usize) -> String {
    let clean: String = value.chars().filter(|c| !c.is_control()).collect();
    let mut end = clean.len().min(limit);
    while !clean.is_char_boundary(end) {
        end -= 1;
    }
    clean[..end].to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn text_is_utf8_bounded_and_has_no_control_characters() {
        assert_eq!(bounded_text("股票\n测试", 7), "股票");
        assert_eq!(bounded_text("a\r\nb", 30), "ab");
    }
}
