//! User-managed watchlist and Tencent quote provider. No secrets, scripts or arbitrary URLs.
//! The native worker remains active when navigating away from Component Center.
use crate::widget_data::{bounded_text, DataRow, DataSnapshot, MAX_ROWS, PROTOCOL};
use chrono::{DateTime, Datelike, NaiveDateTime, NaiveTime, Utc, Weekday};
use serde::{Deserialize, Serialize};
use std::{
    collections::{HashMap, HashSet},
    path::PathBuf,
    sync::{Mutex, OnceLock},
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tokio::sync::Mutex as AsyncMutex;

const SOURCE: &str = "stocks.watchlist";
const INTERVAL_MS: u64 = 2_000;
const RESPONSE_LIMIT: usize = 128 * 1024;
fn refresh_interval_ms(background: bool, failures: u32) -> u64 {
    if background {
        INTERVAL_MS * (1u64 << failures.min(4))
    } else {
        INTERVAL_MS
    }
}
fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Watchlist {
    pub symbols: Vec<String>,
}
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Quote {
    pub symbol: String,
    pub name: String,
    pub price: String,
    pub change_percent: String,
    pub quote_time: String,
    pub currency: String,
    pub stale: bool,
    pub tone: i8,
    pub session_current: bool,
}

fn current_session_quote(q: &Quote, now: DateTime<Utc>) -> bool {
    let zone = if q.symbol.starts_with("us") {
        chrono_tz::America::New_York
    } else {
        chrono_tz::Asia::Shanghai
    };
    let local = now.with_timezone(&zone);
    let open = NaiveTime::from_hms_opt(9, 30, 0).unwrap();
    let timestamp =
        NaiveDateTime::parse_from_str(&q.quote_time.replace('/', "-"), "%Y-%m-%d %H:%M:%S");
    !matches!(local.weekday(), Weekday::Sat | Weekday::Sun)
        && local.time() >= open
        && timestamp.is_ok_and(|ts| ts.date() == local.date_naive() && ts.time() >= open)
}
fn update_quote_tone(q: &mut Quote, now: DateTime<Utc>) {
    q.session_current = current_session_quote(q, now);
    let percent = q.change_percent.parse::<f64>().unwrap_or(0.0);
    q.tone = if q.stale || !q.session_current || !percent.is_finite() {
        0
    } else if percent > 0.0 {
        1
    } else if percent < 0.0 {
        -1
    } else {
        0
    };
}
#[derive(Clone, Debug, Serialize)]
pub struct StockMatch {
    pub symbol: String,
    pub name: String,
    pub market: String,
}

fn parse_search(body: &[u8]) -> Result<Vec<StockMatch>, String> {
    let (text, _, _) = encoding_rs::GBK.decode(body);
    let encoded = text
        .trim()
        .strip_prefix("v_hint=")
        .ok_or("股票检索响应格式异常")?
        .trim()
        .trim_end_matches(';')
        .trim();
    // Upstream returns a JS assignment containing a JSON string. Never evaluate it.
    let hints: String = serde_json::from_str(encoded).map_err(|_| "股票检索响应格式异常")?;
    let mut seen = HashSet::new();
    let mut result = Vec::new();
    for row in hints.split('^') {
        let fields: Vec<_> = row.split('~').collect();
        if fields.len() < 5 || !fields[4].starts_with("GP") || fields[2].is_empty() {
            continue;
        }
        let market = match fields[0] {
            "sh" => "沪市",
            "sz" => "深市",
            "bj" => "北交所",
            "hk" => "港股",
            "us" => "美股",
            _ => continue,
        };
        let mut code = fields[1].to_string();
        if fields[0] == "us" {
            if let Some((ticker, exchange)) = code.rsplit_once('.') {
                if ["n", "oq", "am", "ps", "pk", "ob"]
                    .contains(&exchange.to_ascii_lowercase().as_str())
                {
                    code = ticker.to_string();
                }
            }
        }
        let Ok(symbol) = normalize_symbol(&format!("{}{}", fields[0], code)) else {
            continue;
        };
        if !seen.insert(symbol.clone()) {
            continue;
        }
        result.push(StockMatch {
            symbol,
            name: bounded_text(fields[2], 96),
            market: market.into(),
        });
        if result.len() == 12 {
            break;
        }
    }
    Ok(result)
}

#[tauri::command]
pub async fn stock_search(query: String) -> Result<Vec<StockMatch>, String> {
    let query = query.trim();
    if query.is_empty() {
        return Ok(vec![]);
    }
    if query.chars().count() > 32 || query.chars().any(char::is_control) {
        return Err("请输入 32 个字以内的股票名称或代码".into());
    }
    let client = crate::llm_network::client(Duration::from_secs(8))?;
    let mut response = client
        .get("https://smartbox.gtimg.cn/s3/")
        .query(&[("q", query), ("t", "all")])
        .send()
        .await
        .map_err(|_| "股票检索连接失败，请检查网络后重试")?;
    if !response.status().is_success() {
        return Err("股票检索服务暂不可用，请稍后重试".into());
    }
    if response
        .content_length()
        .is_some_and(|n| n > RESPONSE_LIMIT as u64)
    {
        return Err("股票检索响应过大".into());
    }
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|_| "股票检索下载失败")? {
        if body.len() + chunk.len() > RESPONSE_LIMIT {
            return Err("股票检索响应过大".into());
        }
        body.extend_from_slice(&chunk);
    }
    parse_search(&body)
}
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StockStatus {
    pub settings: Watchlist,
    pub quotes: Vec<Quote>,
    pub error: String,
    pub fetched_at_ms: u64,
    pub refreshing: bool,
}
#[derive(Default)]
struct Service {
    path: PathBuf,
    settings: Watchlist,
    generation: u64,
    quotes: Vec<Quote>,
    error: String,
    fetched_at_ms: u64,
    attempted_at_ms: u64,
    failures: u32,
    refreshing: bool,
}
static SERVICE: OnceLock<Mutex<Service>> = OnceLock::new();
static FETCH_LOCK: AsyncMutex<()> = AsyncMutex::const_new(());
fn service() -> &'static Mutex<Service> {
    SERVICE.get_or_init(|| Mutex::new(Service::default()))
}

pub fn normalize_symbol(raw: &str) -> Result<String, String> {
    let raw = raw.trim();
    let lower = raw.to_ascii_lowercase();
    let valid_digits = |s: &str, n| s.len() == n && s.bytes().all(|b| b.is_ascii_digit());
    if ["sh", "sz", "bj"]
        .iter()
        .any(|p| lower.starts_with(p) && valid_digits(&lower[2..], 6))
        || (lower.starts_with("hk") && valid_digits(&lower[2..], 5))
    {
        return Ok(lower);
    }
    if lower.starts_with("us") {
        let symbol = &raw[2..];
        if !symbol.is_empty()
            && symbol.len() <= 12
            && symbol.as_bytes()[0].is_ascii_alphabetic()
            && symbol
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-')
        {
            return Ok(format!("us{}", symbol.to_ascii_uppercase()));
        }
    }
    Err("请输入带市场的股票代码，例如 sh600000、sz000001、bj920000、hk00700 或 usAAPL".into())
}
fn validate_settings(input: Watchlist) -> Result<Watchlist, String> {
    if input.symbols.len() > MAX_ROWS {
        return Err(format!("最多添加 {MAX_ROWS} 只股票"));
    }
    let mut seen = HashSet::new();
    let mut symbols = Vec::new();
    for raw in input.symbols {
        let symbol = normalize_symbol(&raw)?;
        if !seen.insert(symbol.clone()) {
            return Err("股票列表包含重复代码".into());
        }
        symbols.push(symbol);
    }
    Ok(Watchlist { symbols })
}
pub fn configure(dir: PathBuf) -> Result<(), String> {
    let path = dir.join("stock-watchlist.json");
    *service().lock().map_err(|_| "自选股状态不可用")? = Service {
        path: path.clone(),
        ..Service::default()
    };
    let settings = if path.exists() {
        let metadata = std::fs::metadata(&path).map_err(|_| "无法读取自选股配置")?;
        if metadata.len() > 4096 {
            return Err("自选股配置文件过大".into());
        }
        let bytes = std::fs::read(&path).map_err(|_| "无法读取自选股配置")?;
        validate_settings(
            serde_json::from_slice(&bytes)
                .map_err(|_| "自选股配置损坏，请检查 stock-watchlist.json")?,
        )?
    } else {
        Watchlist { symbols: vec!["hk01810".into(), "hk09988".into()] }
    };
    *service().lock().map_err(|_| "自选股状态不可用")? = Service {
        path,
        settings,
        ..Service::default()
    };
    Ok(())
}
#[tauri::command]
pub fn stock_watchlist_status() -> Result<StockStatus, String> {
    let s = service().lock().map_err(|_| "自选股状态不可用")?;
    let mut quotes = s.quotes.clone();
    if now_ms().saturating_sub(s.fetched_at_ms) > 30_000 {
        for q in &mut quotes {
            q.stale = true;
        }
    }
    let now = Utc::now();
    for quote in &mut quotes {
        update_quote_tone(quote, now);
    }
    Ok(StockStatus {
        settings: s.settings.clone(),
        quotes,
        error: s.error.clone(),
        fetched_at_ms: s.fetched_at_ms,
        refreshing: s.refreshing,
    })
}
#[tauri::command]
pub fn stock_watchlist_save(input: Watchlist) -> Result<StockStatus, String> {
    let settings = validate_settings(input)?;
    {
        let mut s = service().lock().map_err(|_| "自选股状态不可用")?;
        let dir = s
            .path
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .ok_or("自选股存储尚未初始化")?;
        std::fs::create_dir_all(dir).map_err(|_| "无法创建自选股配置目录")?;
        let mut temp = tempfile::NamedTempFile::new_in(dir).map_err(|_| "无法保存自选股")?;
        use std::io::Write;
        temp.write_all(&serde_json::to_vec(&settings).map_err(|_| "自选股配置无效")?)
            .map_err(|_| "无法保存自选股")?;
        temp.as_file().sync_all().map_err(|_| "无法保存自选股")?;
        temp.persist(&s.path).map_err(|_| "无法更新自选股配置")?;
        s.quotes.retain(|q| settings.symbols.contains(&q.symbol));
        s.settings = settings;
        s.generation += 1;
        s.attempted_at_ms = 0;
        s.failures = 0;
        s.error.clear();
    }
    stock_watchlist_status()
}
fn valid_decimal(raw: &str, signed: bool) -> bool {
    if raw.is_empty() || raw.len() > 18 {
        return false;
    }
    let digits = if signed {
        raw.trim_start_matches(['+', '-'])
    } else {
        raw
    };
    let parts: Vec<_> = digits.split('.').collect();
    parts.len() <= 2
        && parts
            .iter()
            .all(|part| !part.is_empty() && part.bytes().all(|b| b.is_ascii_digit()))
        && raw
            .parse::<f64>()
            .is_ok_and(|n| n.is_finite() && (signed || n > 0.0))
}
pub fn parse_quotes(body: &[u8], requested: &[String]) -> Vec<Quote> {
    let (text, _, _) = encoding_rs::GBK.decode(body);
    let allowed: HashSet<_> = requested.iter().map(String::as_str).collect();
    let mut by_symbol = HashMap::new();
    for line in text.split(';') {
        let Some((key, value)) = line.trim().split_once('=') else {
            continue;
        };
        let Some(symbol) = key.trim().strip_prefix("v_") else {
            continue;
        };
        if !allowed.contains(symbol) {
            continue;
        }
        let Some(value) = value
            .trim()
            .strip_prefix('"')
            .and_then(|v| v.strip_suffix('"'))
        else {
            continue;
        };
        let fields: Vec<_> = value.split('~').collect();
        if fields.len() < 33
            || fields[1].is_empty()
            || !valid_decimal(fields[3], false)
            || (!fields[32].is_empty() && !valid_decimal(fields[32], true))
        {
            continue;
        }
        let time = fields[30];
        let quote_time = if time.len() == 14 && time.bytes().all(|b| b.is_ascii_digit()) {
            format!(
                "{}-{}-{} {}:{}:{}",
                &time[..4],
                &time[4..6],
                &time[6..8],
                &time[8..10],
                &time[10..12],
                &time[12..]
            )
        } else if time.len() == 19
            && time.bytes().enumerate().all(|(i, b)| match i {
                4 | 7 => b == b'-' || b == b'/',
                10 => b == b' ',
                13 | 16 => b == b':',
                _ => b.is_ascii_digit(),
            })
        {
            time.to_string()
        } else {
            continue;
        };
        let currency = if symbol.starts_with("hk") {
            "HKD"
        } else if symbol.starts_with("us") {
            "USD"
        } else {
            "CNY"
        };
        by_symbol.insert(
            symbol.to_string(),
            Quote {
                symbol: symbol.into(),
                name: bounded_text(fields[1], 36),
                price: fields[3].into(),
                change_percent: fields[32].into(),
                quote_time,
                currency: currency.into(),
                stale: false,
                tone: 0,
                session_current: false,
            },
        );
    }
    requested
        .iter()
        .filter_map(|s| by_symbol.remove(s))
        .collect()
}
async fn fetch_quotes(symbols: &[String]) -> Result<Vec<Quote>, String> {
    if symbols.is_empty() {
        return Ok(vec![]);
    }
    let client = crate::llm_network::client(Duration::from_secs(8))?;
    let url = format!("https://qt.gtimg.cn/q={}", symbols.join(","));
    let mut response = client
        .get(url)
        .send()
        .await
        .map_err(|_| "行情连接失败，请检查网络后重试")?;
    if !response.status().is_success() {
        return Err(format!(
            "行情服务暂不可用（HTTP {}）",
            response.status().as_u16()
        ));
    }
    if response
        .content_length()
        .is_some_and(|n| n > RESPONSE_LIMIT as u64)
    {
        return Err("行情响应超过大小限制".into());
    }
    let mut body = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|_| "行情下载失败")? {
        if body.len() + chunk.len() > RESPONSE_LIMIT {
            return Err("行情响应超过大小限制".into());
        }
        body.extend_from_slice(&chunk);
    }
    Ok(parse_quotes(&body, symbols))
}
#[tauri::command]
pub async fn stock_quote_lookup(symbol: String) -> Result<Quote, String> {
    let symbol = normalize_symbol(&symbol)?;
    let _guard = FETCH_LOCK.lock().await;
    fetch_quotes(&[symbol])
        .await?
        .into_iter()
        .next()
        .ok_or("未找到有效行情，请检查市场和股票代码".into())
}
#[tauri::command]
pub async fn stock_watchlist_refresh() -> Result<StockStatus, String> {
    refresh(false).await?;
    stock_watchlist_status()
}
async fn refresh(background: bool) -> Result<(), String> {
    let Ok(_guard) = FETCH_LOCK.try_lock() else {
        return Ok(());
    };
    let (symbols, generation) = {
        let mut s = service().lock().map_err(|_| "自选股状态不可用")?;
        let interval = refresh_interval_ms(background, s.failures);
        if s.settings.symbols.is_empty() || now_ms().saturating_sub(s.attempted_at_ms) < interval {
            return Ok(());
        }
        s.attempted_at_ms = now_ms();
        s.refreshing = true;
        (s.settings.symbols.clone(), s.generation)
    };
    let result = fetch_quotes(&symbols).await;
    let mut s = service().lock().map_err(|_| "自选股状态不可用")?;
    apply_fetch_result(&mut s, &symbols, generation, result);
    Ok(())
}
fn apply_fetch_result(
    s: &mut Service,
    symbols: &[String],
    generation: u64,
    result: Result<Vec<Quote>, String>,
) {
    s.refreshing = false;
    if generation != s.generation {
        return;
    } // An edit invalidates in-flight results.
    match result {
        Ok(quotes) => {
            let partial = quotes.len() != symbols.len();
            let mut previous: HashMap<_, _> = s
                .quotes
                .drain(..)
                .map(|mut q| {
                    q.stale = true;
                    (q.symbol.clone(), q)
                })
                .collect();
            for q in quotes {
                previous.insert(q.symbol.clone(), q);
            }
            s.quotes = symbols
                .iter()
                .filter_map(|id| previous.remove(id))
                .collect();
            s.error = if partial {
                "部分股票未返回有效行情，旧数据已标记".into()
            } else {
                String::new()
            };
            s.fetched_at_ms = now_ms();
            s.failures = if partial { (s.failures + 1).min(4) } else { 0 };
        }
        Err(error) => {
            s.error = error;
            s.failures = (s.failures + 1).min(4);
            for q in &mut s.quotes {
                q.stale = true;
            }
        }
    }
}
fn data_snapshot(status: &StockStatus) -> DataSnapshot {
    let rows = status
        .settings
        .symbols
        .iter()
        .map(|symbol| {
            let quote = status.quotes.iter().find(|q| &q.symbol == symbol);
            match quote {
                Some(q) => DataRow {
                    id: symbol.clone(),
                    label: q.name.clone(),
                    value: q.price.clone(),
                    detail: if q.change_percent.is_empty() {
                        "--".into()
                    } else {
                        format!(
                            "{}{}%",
                            if !q.change_percent.starts_with(['-', '+']) {
                                "+"
                            } else {
                                ""
                            },
                            q.change_percent
                        )
                    },
                    meta: bounded_text(
                        &format!(
                            "{} {}{}",
                            symbol,
                            q.currency,
                            if q.stale {
                                " 已过期"
                            } else if !q.session_current {
                                " 非当日盘中报价"
                            } else {
                                ""
                            }
                        ),
                        63,
                    ),
                    tone: q.tone,
                },
                None => DataRow {
                    id: symbol.clone(),
                    label: symbol.clone(),
                    value: "--".into(),
                    detail: "--".into(),
                    meta: "等待有效行情".into(),
                    tone: 0,
                },
            }
        })
        .collect();
    DataSnapshot {
        schema: 1,
        date: Utc::now()
            .with_timezone(&chrono_tz::Asia::Shanghai)
            .format("%Y-%m-%d")
            .to_string(),
        source: SOURCE.into(),
        ttl_ms: 30_000,
        status: if status.settings.symbols.is_empty() {
            "empty"
        } else if !status.error.is_empty() || status.quotes.iter().any(|q| q.stale) {
            "error"
        } else {
            "ok"
        }
        .into(),
        message: if status.settings.symbols.is_empty() {
            "请在 PC 组件中心添加自选股".into()
        } else if !status.error.is_empty() {
            bounded_text(&status.error, 90)
        } else {
            String::new()
        },
        rows,
    }
}
pub fn start(usb: crate::usb_serial::UsbSerialManager) {
    tauri::async_runtime::spawn(async move {
        let mut last_sent = 0;
        let mut last_board = String::new();
        let mut last_generation = u64::MAX;
        loop {
            let _ = refresh(true).await;
            let status = usb.status();
            let generation = service().lock().map(|s| s.generation).unwrap_or(0);
            if status.connected
                && status
                    .capabilities
                    .get("widgetData")
                    .and_then(serde_json::Value::as_str)
                    == Some(PROTOCOL)
                && (status.board_device_id != last_board
                    || generation != last_generation
                    || now_ms().saturating_sub(last_sent) >= INTERVAL_MS)
            {
                if let Ok(snapshot) = stock_watchlist_status() {
                    if let Ok(payload) = serde_json::to_value(data_snapshot(&snapshot)) {
                        let manager = usb.clone();
                        let board = status.board_device_id.clone();
                        let sent = tauri::async_runtime::spawn_blocking(move || {
                            manager.send_widget_data(&board, &payload)
                        })
                        .await;
                        if matches!(sent, Ok(Ok(()))) {
                            last_sent = now_ms();
                            last_board = status.board_device_id;
                            last_generation = generation;
                        }
                    }
                }
            }
            if !status.connected {
                last_board.clear();
            }
            tokio::time::sleep(Duration::from_secs(1)).await;
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn two_second_refresh_keeps_bounded_error_backoff() {
        assert_eq!(INTERVAL_MS, 2_000);
        assert_eq!(refresh_interval_ms(true, 0), 2_000);
        assert_eq!(refresh_interval_ms(true, 1), 4_000);
        assert_eq!(refresh_interval_ms(true, 4), 32_000);
        assert_eq!(refresh_interval_ms(true, u32::MAX), 32_000);
        assert_eq!(refresh_interval_ms(false, 0), 2_000);
        assert_eq!(refresh_interval_ms(false, 4), 2_000);
    }
    #[test]
    fn device_footer_hides_source_but_keeps_empty_and_failure_guidance() {
        let mut status = StockStatus {
            settings: Watchlist { symbols: vec!["sh600000".into()] },
            quotes: parse_quotes(&response("sh600000", "10", "1", "股票"), &["sh600000".into()]),
            error: String::new(),
            fetched_at_ms: 0,
            refreshing: false,
        };
        let data = data_snapshot(&status);
        assert_eq!(data.status, "ok");
        assert!(data.message.is_empty());
        assert_eq!(data.rows.len(), 1);
        status.error = "行情连接失败，请检查网络后重试".into();
        assert_eq!(data_snapshot(&status).message, status.error);
        status.settings.symbols.clear();
        assert_eq!(data_snapshot(&status).message, "请在 PC 组件中心添加自选股");
    }
    #[test]
    fn quote_colors_respect_market_date_opening_time_dst_and_flat_values() {
        let mut q = parse_quotes(
            &response("sh600000", "10", "1.2", "股票"),
            &["sh600000".into()],
        )
        .remove(0);
        let at = |raw: &str| {
            DateTime::parse_from_rfc3339(raw)
                .unwrap()
                .with_timezone(&Utc)
        };
        update_quote_tone(&mut q, at("2026-09-21T06:00:00Z"));
        assert_eq!(q.tone, 1);
        q.change_percent = "-0.7".into();
        update_quote_tone(&mut q, at("2026-09-21T06:00:00Z"));
        assert_eq!(q.tone, -1);
        q.change_percent = "0.00".into();
        update_quote_tone(&mut q, at("2026-09-21T06:00:00Z"));
        assert_eq!(q.tone, 0);
        q.change_percent.clear();
        update_quote_tone(&mut q, at("2026-09-21T06:00:00Z"));
        assert_eq!(q.tone, 0);
        q.change_percent = "1".into();
        update_quote_tone(&mut q, at("2026-09-22T00:00:00Z"));
        assert_eq!(q.tone, 0);
        q.quote_time = "2026-09-22 09:20:00".into();
        update_quote_tone(&mut q, at("2026-09-22T01:25:00Z"));
        assert_eq!(q.tone, 0);
        q.symbol = "usAAPL".into();
        q.quote_time = "2026-09-21 09:31:00".into();
        update_quote_tone(&mut q, at("2026-09-21T13:29:00Z"));
        assert_eq!(q.tone, 0);
        update_quote_tone(&mut q, at("2026-09-21T13:31:00Z"));
        assert_eq!(q.tone, 1);
        q.quote_time = "2026-01-05 09:31:00".into();
        update_quote_tone(&mut q, at("2026-01-05T14:29:00Z"));
        assert_eq!(q.tone, 0);
        update_quote_tone(&mut q, at("2026-01-05T14:31:00Z"));
        assert_eq!(q.tone, 1);
        q.stale = true;
        update_quote_tone(&mut q, at("2026-01-05T14:31:00Z"));
        assert_eq!(q.tone, 0);
        let status = StockStatus {
            settings: Watchlist {
                symbols: vec![q.symbol.clone()],
            },
            quotes: vec![q],
            error: String::new(),
            fetched_at_ms: 0,
            refreshing: false,
        };
        let data = data_snapshot(&status);
        assert_eq!(data.date.len(), 10);
        assert!(!data.rows[0].meta.contains("2026"));
        assert!(!data.rows[0].meta.contains("09:31"));
    }
    #[test]
    fn chinese_search_decodes_json_filters_markets_and_normalizes_us_tickers() {
        let hints = "sh~600519~贵州茅台~gzmt~GP-A^hk~00700~腾讯控股~txkg~GP^us~aapl.oq~苹果~pg~GP^us~brk.b.n~伯克希尔~bk~GP^hk~13005~认购证~rg~QZ^sh~000847~指数~zs~ZS^hk~00700~重复~cf~GP";
        let body = format!("v_hint={};", serde_json::to_string(hints).unwrap());
        let bytes = encoding_rs::GBK.encode(&body).0.into_owned();
        let matches = parse_search(&bytes).unwrap();
        assert_eq!(
            matches
                .iter()
                .map(|x| x.symbol.as_str())
                .collect::<Vec<_>>(),
            ["sh600519", "hk00700", "usAAPL", "usBRK.B"]
        );
        assert_eq!(matches[0].name, "贵州茅台");
        assert_eq!(matches[1].market, "港股");
        assert_eq!(
            parse_search(br#"v_hint="hk~00700~\u817e\u8baf~tx~GP""#).unwrap()[0].name,
            "腾讯"
        );
        assert!(parse_search(b"v_hint=\"\";").unwrap().is_empty());
        assert!(parse_search(b"v_hint=alert(1)").is_err());
        assert!(parse_search(b"v_hint=\"\";alert(1)").is_err());
    }
    #[test]
    #[ignore = "Explicit read-only Chinese search and quote probe"]
    fn live_chinese_stock_search() {
        let dir = tempfile::tempdir().unwrap();
        crate::llm_network::configure_storage_dir(dir.path().to_path_buf()).unwrap();
        for (query, expected) in [
            ("茅台", "sh600519"),
            ("腾讯", "hk00700"),
            ("苹果", "usAAPL"),
        ] {
            let results = tauri::async_runtime::block_on(stock_search(query.into())).unwrap();
            let found = results
                .iter()
                .find(|x| x.symbol == expected)
                .expect("expected stock search match");
            let quote =
                tauri::async_runtime::block_on(stock_quote_lookup(found.symbol.clone())).unwrap();
            assert_eq!(quote.symbol, expected);
        }
        println!("Chinese search → canonical ticker → quote: A/HK/US passed");
    }
    fn response(symbol: &str, price: &str, percent: &str, name: &str) -> Vec<u8> {
        let mut fields = vec![""; 33];
        fields[1] = name;
        fields[3] = price;
        fields[30] = "20260921120546";
        fields[32] = percent;
        encoding_rs::GBK
            .encode(&format!("v_{symbol}=\"{}\";", fields.join("~")))
            .0
            .into_owned()
    }
    #[test]
    fn validates_symbols_and_rejects_url_injection() {
        assert_eq!(normalize_symbol(" Hk00700 ").unwrap(), "hk00700");
        assert_eq!(normalize_symbol("usaapl").unwrap(), "usAAPL");
        for bad in [
            "600000",
            "sh600000,sz000001",
            "usAAPL?q=x",
            "https://localhost",
            "us../etc",
            "hk700",
            "us",
        ] {
            assert!(normalize_symbol(bad).is_err(), "{bad}");
        }
        assert!(validate_settings(Watchlist {
            symbols: vec!["hk00700".into(), "HK00700".into()]
        })
        .is_err());
    }
    #[test]
    fn decodes_gbk_preserves_price_precision_and_provider_time() {
        let q = parse_quotes(
            &response("hk00700", "426.200", "1.72", "腾讯控股"),
            &["hk00700".into()],
        );
        assert_eq!(q[0].name, "腾讯控股");
        assert_eq!(q[0].price, "426.200");
        assert_eq!(q[0].currency, "HKD");
        assert_eq!(q[0].quote_time, "2026-09-21 12:05:46");
    }
    #[test]
    fn rejects_missing_invalid_unrequested_values() {
        for price in ["", "NaN", "inf", "0", "-1", "1e999"] {
            assert!(
                parse_quotes(&response("sh600000", price, "1", "x"), &["sh600000".into()])
                    .is_empty()
            );
        }
        assert!(
            parse_quotes(&response("sh600000", "10", "1", "x"), &["sz000001".into()]).is_empty()
        );
        assert!(parse_quotes(b"v_pv_none_match=\"1\";", &["sh000000".into()]).is_empty());
    }
    #[test]
    fn snapshot_preserves_watchlist_order_and_honest_empty_state() {
        let status = StockStatus {
            settings: Watchlist {
                symbols: vec!["usAAPL".into()],
            },
            quotes: vec![],
            error: String::new(),
            fetched_at_ms: 0,
            refreshing: false,
        };
        let data = data_snapshot(&status);
        assert_eq!(data.rows[0].value, "--");
        assert_eq!(data.rows[0].id, "usAAPL");
    }
    #[test]
    fn settings_roundtrip_is_atomic_ordered_and_bounded() {
        let dir = tempfile::tempdir().unwrap();
        configure(dir.path().to_path_buf()).unwrap();
        assert_eq!(stock_watchlist_status().unwrap().settings.symbols, ["hk01810", "hk09988"]);
        let saved = stock_watchlist_save(Watchlist {
            symbols: vec!["usAAPL".into(), "hk00700".into()],
        })
        .unwrap();
        assert_eq!(saved.settings.symbols, vec!["usAAPL", "hk00700"]);
        configure(dir.path().to_path_buf()).unwrap();
        assert_eq!(
            stock_watchlist_status().unwrap().settings.symbols,
            saved.settings.symbols
        );
        assert!(stock_watchlist_save(Watchlist {
            symbols: vec!["sh600000".into(); 21]
        })
        .is_err());
        assert_eq!(
            stock_watchlist_status().unwrap().settings.symbols,
            saved.settings.symbols
        );
        let files: Vec<_> = std::fs::read_dir(dir.path()).unwrap().collect();
        assert_eq!(files.len(), 1);
        stock_watchlist_save(Watchlist::default()).unwrap();
        configure(dir.path().to_path_buf()).unwrap();
        assert!(stock_watchlist_status()
            .unwrap()
            .settings
            .symbols
            .is_empty());
    }
    #[test]
    fn generated_package_passes_desktop_validation() {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/stock-watchlist");
        let result = crate::clawpkg::validate_clawpkg_at_path(&path).unwrap();
        assert!(result.ok, "{:?}", result.errors);
    }
    #[test]
    fn late_results_cannot_restore_a_removed_stock() {
        let symbols = vec!["sh600000".into()];
        let mut state = Service {
            generation: 2,
            refreshing: true,
            ..Service::default()
        };
        let quotes = parse_quotes(&response("sh600000", "10", "1", "旧股票"), &symbols);
        apply_fetch_result(&mut state, &symbols, 1, Ok(quotes));
        assert!(state.quotes.is_empty());
        assert_eq!(state.fetched_at_ms, 0);
        assert!(!state.refreshing);
    }
    #[test]
    fn partial_and_failed_refresh_keep_old_values_explicitly_stale() {
        let symbols = vec!["sh600000".into(), "hk00700".into()];
        let mut body = response("sh600000", "10", "1", "沪市股票");
        body.extend(response("hk00700", "20", "2", "港股"));
        let mut state = Service {
            quotes: parse_quotes(&body, &symbols),
            ..Service::default()
        };
        let partial = parse_quotes(&response("sh600000", "11", "3", "沪市股票"), &symbols);
        apply_fetch_result(&mut state, &symbols, 0, Ok(partial));
        assert_eq!(state.quotes[0].price, "11");
        assert!(!state.quotes[0].stale);
        assert_eq!(state.quotes[1].price, "20");
        assert!(state.quotes[1].stale);
        assert!(!state.error.is_empty());
        apply_fetch_result(&mut state, &symbols, 0, Err("连接失败".into()));
        assert!(state.quotes.iter().all(|q| q.stale));
        assert_eq!(state.failures, 2);
    }
    #[test]
    #[ignore = "Explicit read-only live Tencent probe; no personal settings or keys"]
    fn live_tencent_probe() {
        let dir = tempfile::tempdir().unwrap();
        crate::llm_network::configure_storage_dir(dir.path().to_path_buf()).unwrap();
        let symbols = vec![
            "sh600000".into(),
            "sz000001".into(),
            "hk00700".into(),
            "usAAPL".into(),
        ];
        let quotes = tauri::async_runtime::block_on(fetch_quotes(&symbols)).unwrap();
        assert_eq!(quotes.len(), 4);
        assert_eq!(
            quotes.iter().map(|q| q.symbol.clone()).collect::<Vec<_>>(),
            symbols
        );
        println!("Tencent HTTPS + GBK + A/HK/US quote parsing: 4 valid records");
    }
}
