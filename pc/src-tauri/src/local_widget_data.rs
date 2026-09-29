//! Local-only telemetry and todo feed. Uses the existing serialized USB writer.
use crate::widget_data::{DataRow, DataSnapshot, PROTOCOL};
use std::{
    sync::{Mutex, OnceLock},
    time::{Duration, Instant},
};
static COMPUTER: OnceLock<Mutex<Option<(Instant, DataSnapshot)>>> = OnceLock::new();
fn cache() -> &'static Mutex<Option<(Instant, DataSnapshot)>> {
    COMPUTER.get_or_init(Default::default)
}
pub fn computer_status() -> DataSnapshot {
    match cache().lock().ok().and_then(|s| s.clone()) {
        Some((at, mut data)) => {
            if at.elapsed() > Duration::from_secs(30) {
                data.status = "error".into();
                data.message = "电脑状态已过期，等待恢复采样".into();
            }
            data
        }
        None => snapshot(vec![], "empty", "正在采样电脑状态"),
    }
}
#[tauri::command]
pub fn local_widget_snapshot(source: String) -> Result<DataSnapshot, String> {
    match source.as_str() {
        "computer.status" => Ok(computer_status()),
        "todos.upcoming" => Ok(crate::todos::snapshot()),
        _ => Err("不支持的数据源".into()),
    }
}
fn snapshot(rows: Vec<DataRow>, status: &str, message: &str) -> DataSnapshot {
    DataSnapshot {
        schema: 1,
        date: chrono::Local::now().format("%Y-%m-%d").to_string(),
        source: "computer.status".into(),
        ttl_ms: 30_000,
        status: status.into(),
        message: message.into(),
        rows,
    }
}
fn rate(bytes: u64, seconds: f64) -> String {
    if seconds <= 0.0 || !seconds.is_finite() {
        return "—".into();
    }
    let kib = bytes as f64 / seconds / 1024.0;
    if kib >= 1024.0 {
        format!("{:.1} MiB/s", kib / 1024.0)
    } else {
        format!("{kib:.1} KiB/s")
    }
}
fn row(id: &str, label: &str, value: String, detail: String, meta: &str) -> DataRow {
    DataRow {
        id: id.into(),
        label: label.into(),
        value: crate::widget_data::bounded_text(&value, 24),
        detail: crate::widget_data::bounded_text(&detail, 24),
        meta: meta.into(),
        tone: 0,
    }
}
struct Sampler {
    system: sysinfo::System,
    networks: sysinfo::Networks,
    sampled: Instant,
}
impl Sampler {
    fn new() -> Self {
        let mut system = sysinfo::System::new();
        system.refresh_cpu_usage();
        Self {
            system,
            networks: sysinfo::Networks::new_with_refreshed_list(),
            sampled: Instant::now(),
        }
    }
    fn sample(&mut self) -> DataSnapshot {
        let elapsed = self.sampled.elapsed().as_secs_f64();
        self.sampled = Instant::now();
        self.system.refresh_cpu_usage();
        self.system.refresh_memory();
        self.networks.refresh(true);
        let down = self
            .networks
            .values()
            .fold(0u64, |n, i| n.saturating_add(i.received()));
        let up = self
            .networks
            .values()
            .fold(0u64, |n, i| n.saturating_add(i.transmitted()));
        let total = self.system.total_memory();
        let used = self.system.used_memory();
        let memory = if total > 0 {
            format!("{:.0}%", used as f64 / total as f64 * 100.0)
        } else {
            "—".into()
        };
        let rows = vec![
            row(
                "cpu",
                "CPU",
                format!("{:.0}%", self.system.global_cpu_usage()),
                String::new(),
                "处理器总占用",
            ),
            row(
                "memory",
                "内存",
                memory,
                format!(
                    "{:.1}/{:.1} GiB",
                    used as f64 / 1073741824.0,
                    total as f64 / 1073741824.0
                ),
                "已用 / 总量",
            ),
            row(
                "down",
                "网络接收",
                rate(down, elapsed),
                String::new(),
                "全部网络接口合计",
            ),
            row(
                "up",
                "网络发送",
                rate(up, elapsed),
                String::new(),
                "含虚拟接口，非测速",
            ),
            row(
                "uptime",
                "开机时长",
                format!(
                    "{}h {}m",
                    sysinfo::System::uptime() / 3600,
                    sysinfo::System::uptime() % 3600 / 60
                ),
                String::new(),
                "电脑启动至今",
            ),
        ];
        snapshot(rows, "ok", "")
    }
}
pub fn start(usb: crate::usb_serial::UsbSerialManager) {
    // Sampling runs away from async audio/network workers and never enumerates processes.
    std::thread::Builder::new()
        .name("local-widget-sampler".into())
        .spawn(move || {
            let mut sampler = Sampler::new();
            loop {
                std::thread::sleep(Duration::from_secs(2));
                let sample = sampler.sample();
                if let Ok(mut data) = cache().lock() {
                    *data = Some((Instant::now(), sample.clone()));
                }
                let status = usb.status();
                if !status.connected
                    || status
                        .capabilities
                        .get("widgetData")
                        .and_then(serde_json::Value::as_str)
                        != Some(PROTOCOL)
                {
                    continue;
                }
                for snapshot in [sample, crate::todos::snapshot()] {
                    if let Ok(payload) = serde_json::to_value(snapshot) {
                        // send_widget_data skips busy asset transfers; next tick retries.
                        let _ = usb.send_widget_data(&status.board_device_id, &payload);
                    }
                }
            }
        })
        .expect("start local widget sampler");
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rate_uses_measured_interval() {
        assert_eq!(rate(2048, 2.0), "1.0 KiB/s");
        assert_eq!(rate(2097152, 2.0), "1.0 MiB/s");
        assert_eq!(rate(1, 0.0), "—");
    }
    #[test]
    fn local_sample_is_bounded_and_contains_no_identity() {
        let result = Sampler::new().sample();
        assert_eq!(result.rows.len(), 5);
        for r in result.rows {
            assert!(
                r.id.len() <= 23
                    && r.label.len() <= 36
                    && r.value.len() <= 24
                    && r.detail.len() <= 24
                    && r.meta.len() <= 63
            );
        }
    }
}
