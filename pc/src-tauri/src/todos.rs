//! One private, atomic store shared by the UI, voice tools and live widget.
use crate::widget_data::{bounded_text, DataRow, DataSnapshot};
use chrono::{DateTime, Local, Utc};
use serde::{Deserialize, Serialize};
use std::{
    io::Write,
    path::PathBuf,
    sync::{Mutex, OnceLock},
};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Todo {
    pub id: String,
    pub title: String,
    pub due_at: Option<String>,
    pub completed: bool,
    pub revision: u64,
    pub created_at: String,
}
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Document {
    items: Vec<Todo>,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Input {
    pub operation: String,
    pub id: Option<String>,
    pub revision: Option<u64>,
    pub title: Option<String>,
    /// Empty string explicitly clears the deadline; omission preserves it.
    pub due_at: Option<String>,
    pub filter: Option<String>,
    pub query: Option<String>,
}
#[derive(Default)]
struct Store {
    path: PathBuf,
    doc: Document,
    error: Option<String>,
}
static STORE: OnceLock<Mutex<Store>> = OnceLock::new();
fn store() -> &'static Mutex<Store> {
    STORE.get_or_init(Default::default)
}
fn title(raw: &str) -> Result<String, String> {
    let s = raw.trim();
    if s.is_empty() || s.chars().count() > 120 || s.chars().any(char::is_control) {
        return Err("待办标题需为 1–120 个字，不能包含控制字符".into());
    }
    Ok(s.into())
}
fn deadline(raw: &str) -> Result<Option<String>, String> {
    if raw.is_empty() {
        return Ok(None);
    }
    let date = DateTime::parse_from_rfc3339(raw)
        .map_err(|_| "截止时间必须包含日期、时间和时区；不明确时请询问用户")?;
    Ok(Some(date.with_timezone(&Utc).to_rfc3339()))
}
fn validate(doc: &Document) -> Result<(), String> {
    if doc.items.len() > 500 {
        return Err("最多保存 500 条待办，请删除不需要的已完成事项".into());
    }
    let mut seen = std::collections::HashSet::new();
    for item in &doc.items {
        title(&item.title)?;
        if item.id.len() != 20
            || !item.id.bytes().all(|b| b.is_ascii_hexdigit())
            || !seen.insert(&item.id)
            || item.revision == 0
        {
            return Err("待办文件包含无效标识或版本".into());
        }
        if let Some(due) = &item.due_at {
            deadline(due)?;
        }
        DateTime::parse_from_rfc3339(&item.created_at).map_err(|_| "待办创建时间无效")?;
    }
    Ok(())
}
impl Store {
    fn apply_tool(&mut self, input: Input) -> Result<Vec<Todo>, String> {
        if input.operation == "list" {
            return self.apply(input);
        }
        let before: std::collections::HashMap<_, _> = self
            .doc
            .items
            .iter()
            .map(|i| (i.id.clone(), i.revision))
            .collect();
        // Voice write receipts disclose only affected records, not unrelated todos.
        Ok(self
            .apply(input)?
            .into_iter()
            .filter(|i| before.get(&i.id) != Some(&i.revision))
            .collect())
    }
    fn load(path: PathBuf) -> Self {
        let mut s = Self {
            path,
            ..Default::default()
        };
        let result = (|| {
            if !s.path.exists() {
                return Ok(Document::default());
            }
            if std::fs::metadata(&s.path)
                .map_err(|_| "无法读取待办")?
                .len()
                > 1024 * 1024
            {
                return Err("待办文件过大".into());
            }
            let doc: Document =
                serde_json::from_slice(&std::fs::read(&s.path).map_err(|_| "无法读取待办")?)
                    .map_err(|_| "待办文件损坏，未覆盖原文件")?;
            validate(&doc)?;
            Ok::<_, String>(doc)
        })();
        match result {
            Ok(doc) => s.doc = doc,
            Err(e) => s.error = Some(e),
        }
        s
    }
    fn apply(&mut self, input: Input) -> Result<Vec<Todo>, String> {
        if let Some(e) = &self.error {
            return Err(e.clone());
        }
        if self.path.as_os_str().is_empty() {
            return Err("待办存储尚未就绪".into());
        }
        if input.operation == "list" {
            let filter = input.filter.as_deref().unwrap_or("pending");
            if !matches!(filter, "pending" | "completed" | "all") {
                return Err("无效待办筛选".into());
            }
            let items = sorted(&self.doc.items, filter);
            let query = input.query.as_deref().unwrap_or("").trim();
            if query.chars().count() > 120 {
                return Err("待办搜索词过长".into());
            }
            return Ok(items
                .into_iter()
                .filter(|i| i.title.contains(query))
                .collect());
        }
        let mut next = self.doc.clone();
        if input.operation == "add" {
            next.items.push(Todo {
                id: uuid::Uuid::new_v4().simple().to_string()[..20].into(),
                title: title(input.title.as_deref().ok_or("请提供待办内容")?)?,
                due_at: deadline(input.due_at.as_deref().unwrap_or(""))?,
                completed: false,
                revision: 1,
                created_at: Utc::now().to_rfc3339(),
            });
        } else {
            let index = next
                .items
                .iter()
                .position(|i| Some(i.id.as_str()) == input.id.as_deref())
                .ok_or("找不到这条待办，请先查询并确认唯一事项")?;
            let item = &mut next.items[index];
            if input.revision != Some(item.revision) {
                return Err("待办已变化，请重新查询后操作；未覆盖新修改".into());
            }
            match input.operation.as_str() {
                "update" => {
                    if input.title.is_none() && input.due_at.is_none() {
                        return Err("请提供要修改的内容或截止时间".into());
                    }
                    if let Some(s) = input.title {
                        item.title = title(&s)?;
                    }
                    if let Some(s) = input.due_at {
                        item.due_at = deadline(&s)?;
                    }
                    item.revision += 1;
                }
                "complete" | "reopen" => {
                    item.completed = input.operation == "complete";
                    item.revision += 1;
                }
                "delete" => {
                    next.items.remove(index);
                }
                _ => return Err("不支持的待办操作".into()),
            }
        }
        validate(&next)?;
        let dir = self.path.parent().ok_or("待办目录不可用")?;
        std::fs::create_dir_all(dir).map_err(|_| "无法创建待办目录")?;
        // NamedTempFile is owner-only on Unix; atomic replacement also works on Windows.
        let mut temp = tempfile::NamedTempFile::new_in(dir).map_err(|_| "无法保存待办")?;
        temp.write_all(&serde_json::to_vec(&next).map_err(|_| "待办编码失败")?)
            .map_err(|_| "无法保存待办")?;
        temp.as_file().sync_all().map_err(|_| "无法保存待办")?;
        temp.persist(&self.path)
            .map_err(|_| "无法更新待办，原数据保留")?;
        self.doc = next;
        Ok(sorted(&self.doc.items, "all"))
    }
}
fn sorted(items: &[Todo], filter: &str) -> Vec<Todo> {
    let mut items: Vec<_> = items
        .iter()
        .filter(|i| filter == "all" || i.completed == (filter == "completed"))
        .cloned()
        .collect();
    items.sort_by(|a, b| {
        a.completed
            .cmp(&b.completed)
            .then_with(|| match (&a.due_at, &b.due_at) {
                (Some(a), Some(b)) => a.cmp(b),
                (Some(_), None) => std::cmp::Ordering::Less,
                (None, Some(_)) => std::cmp::Ordering::Greater,
                _ => std::cmp::Ordering::Equal,
            })
            .then(a.created_at.cmp(&b.created_at))
            .then(a.id.cmp(&b.id))
    });
    items
}
pub fn configure(dir: PathBuf) {
    *store().lock().unwrap() = Store::load(dir.join("todos.json"));
}
pub fn execute(input: Input) -> Result<Vec<Todo>, String> {
    store().lock().map_err(|_| "待办状态不可用")?.apply(input)
}
pub fn execute_tool(input: Input) -> Result<Vec<Todo>, String> {
    store()
        .lock()
        .map_err(|_| "待办状态不可用")?
        .apply_tool(input)
}
pub fn snapshot() -> DataSnapshot {
    let result = execute(Input {
        operation: "list".into(),
        id: None,
        revision: None,
        title: None,
        due_at: None,
        filter: Some("all".into()),
        query: None,
    });
    snapshot_from(result)
}
fn snapshot_from(result: Result<Vec<Todo>, String>) -> DataSnapshot {
    let (items, error) = match result {
        Ok(items) => (items, String::new()),
        Err(e) => (vec![], e),
    };
    let rows = items
        .iter()
        .take(20)
        .map(|item| {
            let due = item
                .due_at
                .as_deref()
                .and_then(|s| DateTime::parse_from_rfc3339(s).ok());
            let overdue = due.is_some_and(|d| d.with_timezone(&Utc) < Utc::now());
            DataRow {
                id: item.id.clone(),
                label: bounded_text(&item.title, 36),
                value: due
                    .map(|d| d.with_timezone(&Local).format("%m-%d %H:%M").to_string())
                    .unwrap_or_else(|| "未定日期".into()),
                detail: if item.completed { "已完成" } else if overdue { "已逾期" } else { "待完成" }.into(),
                meta: bounded_text(&item.title[bounded_text(&item.title, 36).len()..], 63),
                tone: if item.completed { -1 } else { 1 },
            }
        })
        .collect();
    DataSnapshot {
        schema: 1,
        source: "todos.upcoming".into(),
        date: Local::now().format("%Y-%m-%d").to_string(),
        ttl_ms: 30_000,
        status: if !error.is_empty() {
            "error"
        } else if items.is_empty() {
            "empty"
        } else {
            "ok"
        }
        .into(),
        message: if !error.is_empty() {
            bounded_text(&error, 90)
        } else if items.is_empty() {
            "可以对我说：添加一个待办".into()
        } else if items.len() > 20 {
            format!("显示前 20 项，共 {} 项", items.len())
        } else {
            String::new()
        },
        rows,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn input(v: serde_json::Value) -> Input {
        serde_json::from_value(v).unwrap()
    }
    #[test]
    fn crud_conflict_reload_and_empty_persistence() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("todos.json");
        let mut s = Store::load(path.clone());
        let items=s.apply(input(serde_json::json!({"operation":"add","title":"买牛奶","dueAt":"2026-09-25T09:00:00+08:00"}))).unwrap();
        let id = &items[0].id;
        assert_eq!(
            items[0].due_at.as_deref(),
            Some("2026-09-25T01:00:00+00:00")
        );
        assert!(s
            .apply(input(
                serde_json::json!({"operation":"delete","id":id,"revision":9})
            ))
            .is_err());
        s.apply(input(
            serde_json::json!({"operation":"complete","id":id,"revision":1}),
        ))
        .unwrap();
        assert!(Store::load(path.clone()).doc.items[0].completed);
        s.apply(input(serde_json::json!({"operation":"update","id":id,"revision":2,"dueAt":"","title":"买面包"}))).unwrap();
        assert!(s.doc.items[0].due_at.is_none());
        s.apply(input(
            serde_json::json!({"operation":"delete","id":id,"revision":3}),
        ))
        .unwrap();
        assert!(Store::load(path).doc.items.is_empty());
    }
    #[test]
    fn widget_contains_pending_and_completed_with_small_yearless_dates() {
        let dir = tempfile::tempdir().unwrap();
        let mut s = Store::load(dir.path().join("todos.json"));
        let done = s.apply(input(serde_json::json!({"operation":"add","title":"已经办理","dueAt":"2020-09-25T09:30:00+08:00"}))).unwrap()[0].clone();
        s.apply(input(serde_json::json!({"operation":"complete","id":done.id,"revision":1}))).unwrap();
        s.apply_tool(input(serde_json::json!({"operation":"add","title":"买火车票"}))).unwrap();
        let persisted = Store::load(s.path.clone());
        let snap = snapshot_from(Ok(sorted(&persisted.doc.items, "all")));
        assert_eq!(snap.rows.len(), 2);
        assert_eq!(snap.rows[0].label, "买火车票");
        assert_eq!(snap.rows[0].tone, 1);
        assert_eq!(snap.rows[0].detail, "待完成");
        assert_eq!(snap.rows[1].tone, -1);
        assert_eq!(snap.rows[1].detail, "已完成");
        assert_eq!(snap.rows[1].value.len(), 11);
        assert!(!snap.rows[1].value.contains("2020"));
        s.apply(input(serde_json::json!({"operation":"reopen","id":done.id,"revision":2}))).unwrap();
        assert!(snapshot_from(Ok(sorted(&s.doc.items,"all"))).rows.iter().all(|r|r.tone==1));
    }
    #[test]
    fn invalid_inputs_and_corrupt_storage_never_overwrite() {
        assert!(title("\n").is_err());
        assert!(deadline("明天").is_err());
        assert!(deadline("2026-09-25T09:00:00").is_err());
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("todos.json");
        std::fs::write(&path, b"broken").unwrap();
        let mut s = Store::load(path.clone());
        assert!(s
            .apply(input(serde_json::json!({"operation":"add","title":"test"})))
            .is_err());
        assert_eq!(std::fs::read(path).unwrap(), b"broken");
    }
    #[test]
    fn ordering_query_duplicates_and_widget_bounds() {
        let dir = tempfile::tempdir().unwrap();
        let mut s = Store::load(dir.path().join("todos.json"));
        s.apply(input(serde_json::json!({"operation":"add","title":"同名"})))
            .unwrap();
        s.apply(input(serde_json::json!({"operation":"add","title":"同名","dueAt":"2026-09-25T09:00:00+08:00"}))).unwrap();
        let items = s
            .apply(input(
                serde_json::json!({"operation":"list","query":"同名"}),
            ))
            .unwrap();
        assert_eq!(items.len(), 2);
        assert_ne!(items[0].id, items[1].id);
        assert!(items[0].due_at.is_some());
        assert!(s
            .apply(input(
                serde_json::json!({"operation":"delete","title":"同名"})
            ))
            .is_err());
        let mut long = items[0].clone();
        long.title = "超长待办".repeat(25);
        let snapshot = snapshot_from(Ok(vec![long; 30]));
        assert_eq!(snapshot.rows.len(), 20);
        assert!(snapshot.message.contains("30"));
        for row in snapshot.rows {
            assert!(
                row.label.len() <= 36
                    && row.meta.len() <= 63
                    && row.value.len() <= 24
                    && row.detail.len() <= 24
            );
        }
        assert_eq!(snapshot_from(Ok(vec![])).status, "empty");
        assert_eq!(snapshot_from(Err("错误".repeat(100))).message.len(), 90);
    }
    #[test]
    fn voice_mutation_receipts_do_not_disclose_unrelated_items() {
        let dir = tempfile::tempdir().unwrap();
        let mut s = Store::load(dir.path().join("todos.json"));
        s.apply(input(
            serde_json::json!({"operation":"add","title":"private unrelated item"}),
        ))
        .unwrap();
        let receipt = s
            .apply_tool(input(
                serde_json::json!({"operation":"add","title":"new item"}),
            ))
            .unwrap();
        assert_eq!(receipt.len(), 1);
        assert_eq!(receipt[0].title, "new item");
        let receipt = s
            .apply_tool(input(
                serde_json::json!({"operation":"delete","id":receipt[0].id,"revision":1}),
            ))
            .unwrap();
        assert!(receipt.is_empty());
        assert_eq!(s.doc.items.len(), 1);
    }
    #[test]
    fn failed_save_does_not_change_memory_and_private_file_mode() {
        let dir = tempfile::tempdir().unwrap();
        let blocked = dir.path().join("not-a-directory");
        std::fs::write(&blocked, b"x").unwrap();
        let mut s = Store {
            path: blocked.join("todos.json"),
            ..Default::default()
        };
        assert!(s
            .apply(input(serde_json::json!({"operation":"add","title":"test"})))
            .is_err());
        assert!(s.doc.items.is_empty());
        let mut s = Store::load(dir.path().join("todos.json"));
        s.apply(input(
            serde_json::json!({"operation":"add","title":"private"}),
        ))
        .unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(s.path).unwrap().permissions().mode() & 0o777,
                0o600
            );
        }
    }
}
