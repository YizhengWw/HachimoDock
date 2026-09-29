//! Bounded LRC parsing; lyrics remain in memory, never diagnostics or assets.
use serde::Serialize;
#[derive(Clone, Debug, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub(super) struct Line { pub at_ms: u32, pub text: String }
fn timestamp(tag: &str) -> Option<i64> {
    let (minutes, seconds) = tag.split_once(':')?;
    let minutes = minutes.parse::<u32>().ok()?;
    let seconds = seconds.parse::<f64>().ok()?;
    if minutes > 60 || !seconds.is_finite() || !(0.0..60.0).contains(&seconds) { return None; }
    Some(i64::from(minutes) * 60_000 + (seconds * 1000.0).round() as i64)
}
pub(super) fn parse(lrc: &str) -> Vec<Line> {
    if lrc.len() > 256 * 1024 { return vec![]; }
    let offset = lrc.lines().filter_map(|line| line.trim().strip_prefix("[offset:")?.strip_suffix(']')?.parse::<i64>().ok())
        .last().unwrap_or(0).clamp(-3_600_000, 3_600_000);
    let mut lines = Vec::new();
    for raw in lrc.lines().take(2048) {
        let mut rest = raw.trim();
        let mut times = Vec::new();
        while let Some(tagged) = rest.strip_prefix('[') {
            let Some((tag, tail)) = tagged.split_once(']') else { break };
            if let Some(time) = timestamp(tag) { if times.len() < 16 { times.push(time); } }
            rest = tail;
        }
        let clean: String = rest.chars().filter(|c| !c.is_control()).collect();
        let text = crate::widget_data::bounded_text(clean.trim(), 180);
        // Empty timestamped lines retain instrumental gaps.
        for time in times {
            lines.push(Line { at_ms: time.saturating_add(offset).clamp(0, 3_600_000) as u32, text: text.clone() });
        }
    }
    lines.sort_by_key(|line| line.at_ms);
    lines.dedup_by(|a, b| a.at_ms == b.at_ms);
    lines.truncate(256);
    if lines.iter().all(|line| line.text.is_empty()) { lines.clear(); }
    lines
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn timestamps_offsets_order_and_gaps() {
        let parsed = parse("[ar:test]\n[offset:-100]\n[00:04.2]第二句\n[00:01.00][00:03.000]第一句\n[00:02.00]\n[00:61.0]bad");
        assert_eq!(parsed.iter().map(|l| l.at_ms).collect::<Vec<_>>(), [900, 1900, 2900, 4100]);
        assert!(parsed[1].text.is_empty());assert_eq!(parsed[2].text, "第一句");
    }
    #[test]
    fn malformed_plain_and_unbounded_lyrics_do_not_become_fake_timing() {
        assert!(parse("plain untimed lyrics").is_empty());
        assert!(parse("[00:NaN]bad\n[00:inf]bad\n[00:01]\n[offset:999999999999999999999]").is_empty());
        assert_eq!(parse(&format!("[00:01]{}", "汉".repeat(1000)))[0].text.len(), 180);
        let many = (0..400).map(|i| format!("[{}:{:02}]line\n", i/60, i%60)).collect::<String>();
        assert_eq!(parse(&many).len(), 256);
    }
}
