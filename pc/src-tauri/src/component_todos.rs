//! Device checklist over the same atomic store as desktop and voice tools.
//! Packages own layout; this service only exposes bounded text and safe operations.
use crate::todos::{Input, Todo};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

const ROWS: [&str; 5] = ["row_a", "row_b", "row_c", "row_d", "row_e"];
const SLOTS: [&str; 5] = ["eyebrow", "headline", "metricLabel", "note", "footer"];

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub(crate) struct Reading {
    selected: String,
    completed: bool,
    detail: bool,
    page: usize,
    text_page: usize,
    // Rebuilt from the real store, not trusted from persisted state.
    #[serde(skip)]
    pub(crate) ui: Value,
}

impl PartialEq for Reading {
    fn eq(&self, other: &Self) -> bool {
        self.selected == other.selected && self.completed == other.completed && self.detail == other.detail
            && self.page == other.page && self.text_page == other.text_page
    }
}

fn input(operation: &str) -> Input {
    Input { operation: operation.into(), id: None, revision: None, title: None,
        due_at: None, filter: Some("all".into()), query: None }
}

fn token(todo: &Todo) -> String {
    format!("{:x}", Sha256::digest(serde_json::to_vec(todo).expect("serializable todo")))
}

/// At most 36 Unicode scalars / 144 UTF-8 bytes. Two blocks form one device page.
/// The entire 120-character title is preserved, including four-byte characters.
fn blocks(title: &str) -> Vec<String> {
    title.chars().collect::<Vec<_>>().chunks(36).map(|c| c.iter().collect()).collect()
}

fn short(title: &str, max: usize) -> String {
    if title.chars().count() <= max { title.into() }
    else { format!("{}…", title.chars().take(max - 1).collect::<String>()) }
}

fn due(todo: &Todo, detail: bool) -> String {
    todo.due_at.as_deref().and_then(|s| chrono::DateTime::parse_from_rfc3339(s).ok())
        .map(|d| d.with_timezone(&chrono::Local).format(if detail { "%m-%d %H:%M" } else { "%m-%d" }).to_string())
        .unwrap_or_else(|| "未定期".into())
}

pub(crate) fn execute(reading: &mut Reading, op: &str, request: &Value) -> Result<Value, String> {
    apply(reading, op, request, &mut crate::todos::execute)
}

/// Hardware acceptance uses the production state machine and atomic store,
/// but must never read or change the user's desktop checklist.
#[cfg(test)]
pub(crate) fn test_execute(reading: &mut Reading, op: &str, request: &Value,
    path: &std::path::Path) -> Result<Value, String> {
    apply(reading, op, request, &mut crate::todos::isolated_store(path.to_path_buf()))
}

fn apply(reading: &mut Reading, op: &str, request: &Value,
    store: &mut impl FnMut(Input) -> Result<Vec<Todo>, String>) -> Result<Value, String> {
    let mut all = store(input("list"))?;
    let mut s = reading.clone();
    let mut notice = String::new();
    if op == "toggle" {
        let todo = all.iter().find(|t| t.id == s.selected).ok_or("这条待办已删除，请返回清单")?;
        // Never resolve a write by row number or by title. Confirm the exact version
        // displayed, then use the store's atomic compare-and-swap revision check.
        if !s.detail || request["confirm"] != true || request["context"] != token(todo) {
            return Err("待办内容或选择已变化，请重新查看并确认；未修改事项".into());
        }
        let mut change = input(if todo.completed { "reopen" } else { "complete" });
        change.id = Some(todo.id.clone()); change.revision = Some(todo.revision);
        all = store(change)?;
    }
    if op == "filter" {
        s.completed = !s.completed; s.detail = false; s.selected.clear(); s.page = 0; s.text_page = 0;
    }
    let rows: Vec<_> = all.iter().filter(|t| t.completed == s.completed).collect();
    if s.detail && !all.iter().any(|t| t.id == s.selected) {
        s.detail = false; s.selected.clear(); notice = "原事项已删除".into();
    }
    if !s.detail {
        s.page = s.page.min(rows.len().saturating_sub(1) / 5);
        let mut at = rows.iter().position(|t| t.id == s.selected).unwrap_or(s.page * 5);
        match op {
            "up" => at = at.saturating_sub(1),
            "down" => at = (at + 1).min(rows.len().saturating_sub(1)),
            "previous" => at = (at / 5).saturating_sub(1) * 5,
            "next" => at = ((at / 5 + 1) * 5).min(rows.len().saturating_sub(1)),
            _ => {}
        }
        if let Some(slot) = ROWS.iter().position(|r| *r == op) {
            at = (at / 5) * 5 + slot;
            if at >= rows.len() { return Err("这行已没有事项，请刷新后重试".into()); }
            s.detail = true; s.text_page = 0;
        } else if op == "open" && !rows.is_empty() {
            s.detail = true; s.text_page = 0;
        }
        s.selected = rows.get(at).map(|t| t.id.clone()).unwrap_or_default(); s.page = at / 5;
    } else if op == "open" {
        s.detail = false; s.text_page = 0;
        let at = rows.iter().position(|t| t.id == s.selected).unwrap_or(0);
        s.selected = rows.get(at).map(|t| t.id.clone()).unwrap_or_default(); s.page = at / 5;
    } else {
        let count = all.iter().find(|t| t.id == s.selected).map(|t| blocks(&t.title).len().div_ceil(2)).unwrap_or(1);
        s.text_page = s.text_page.min(count - 1);
        if op == "previous" { s.text_page = s.text_page.saturating_sub(1); }
        if op == "next" { s.text_page = (s.text_page + 1).min(count - 1); }
    }
    let mut values = json!({"eyebrow":"", "headline":"", "metricValue":"", "metricLabel":"", "metricUnit":"", "badge":"", "note":"", "footer":""});
    let mut allowed = vec!["filter"];
    let (mode, context) = if s.detail {
        let todo = all.iter().find(|t| t.id == s.selected).ok_or("事项不存在，请刷新清单")?;
        let chunks = blocks(&todo.title); let pages = chunks.len().div_ceil(2);
        s.text_page = s.text_page.min(pages - 1);
        values["eyebrow"] = format!("{} · {}", if todo.completed { "已完成" } else { "待完成" }, due(todo, true)).into();
        values["headline"] = chunks[s.text_page * 2].clone().into();
        values["note"] = chunks.get(s.text_page * 2 + 1).cloned().unwrap_or_default().into();
        values["metricValue"] = "事项全文".into();
        values["metricUnit"] = if todo.completed { "撤销完成" } else { "标记完成" }.into();
        values["badge"] = format!("{}/{}", s.text_page + 1, pages).into();
        allowed.extend(["open", "toggle"]);
        if s.text_page > 0 { allowed.push("previous"); }
        if s.text_page + 1 < pages { allowed.push("next"); }
        (if todo.completed { 5 } else { 1 }, token(todo))
    } else {
        let count = rows.len(); let pages = count.div_ceil(5).max(1);
        values["metricValue"] = format!("{} · {}", if s.completed { "已完成" } else { "待完成" }, count).into();
        values["badge"] = format!("{}/{}", s.page + 1, pages).into();
        if count == 0 {
            values["headline"] = if notice.is_empty() { if s.completed { "还没有已完成事项" } else { "眼前清单已清空" }.into() } else { notice }.into();
            values["note"] = "可在 PC 或实时对话中添加待办".into();
        } else {
            for (n, todo) in rows.iter().skip(s.page * 5).take(5).enumerate() {
                values[SLOTS[n]] = format!("{}{} · {}", if todo.id == s.selected { "> " } else { "  " }, short(&todo.title, 20), due(todo, false)).into();
                allowed.push(ROWS[n]);
            }
            let at = rows.iter().position(|t| t.id == s.selected).unwrap_or(0);
            allowed.push("open");
            if at > 0 { allowed.push("up"); }
            if at + 1 < count { allowed.push("down"); }
            if s.page > 0 { allowed.push("previous"); }
            if s.page + 1 < pages { allowed.push("next"); }
        }
        (if count == 0 { 2 } else { 0 }, String::new())
    };
    s.ui = json!({"mode":mode,"operations":allowed,"context":context});
    *reading = s;
    Ok(values)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn item(n: usize, title: &str) -> Todo {
        Todo { id: format!("{n:020x}"), title: title.into(), due_at: None, completed: false,
            revision: 1, created_at: "2026-10-21T00:00:00Z".into() }
    }
    fn call(s: &mut Reading, all: &mut Vec<Todo>, op: &str, request: Value) -> Result<Value, String> {
        apply(s, op, &request, &mut |i| {
            if i.operation != "list" {
                let todo = all.iter_mut().find(|t| Some(&t.id) == i.id.as_ref()).unwrap();
                if i.revision != Some(todo.revision) { return Err("stale".into()); }
                todo.completed = i.operation == "complete"; todo.revision += 1;
            }
            Ok(all.clone())
        })
    }
    #[test]
    fn full_title_is_lossless_and_bounded_even_with_emoji() {
        for title in ["知".repeat(120), "🐶".repeat(120), "abc你好🐶".repeat(20)] {
            let mut s = Reading::default(); let mut all = vec![item(1, &title)];
            let first = call(&mut s, &mut all, "open", json!({})).unwrap();
            let mut result = first["headline"].as_str().unwrap().to_string() + first["note"].as_str().unwrap();
            while s.ui["operations"].as_array().unwrap().contains(&json!("next")) {
                let v = call(&mut s, &mut all, "next", json!({})).unwrap();
                assert!(v["headline"].as_str().unwrap().len() <= 144);
                result += v["headline"].as_str().unwrap(); result += v["note"].as_str().unwrap();
            }
            assert_eq!(result, title);
        }
    }
    #[test]
    fn confirmation_is_exact_and_completion_can_be_undone_without_losing_selection() {
        let mut s = Reading::default(); let mut all = vec![item(1, "买菜"), item(2, "写作")];
        call(&mut s, &mut all, "row_b", json!({})).unwrap();
        let old = s.ui["context"].clone();
        assert!(call(&mut s, &mut all, "toggle", json!({"context":old})).is_err());
        let done = call(&mut s, &mut all, "toggle", json!({"confirm":true,"context":old})).unwrap();
        assert_eq!(done["metricUnit"], "撤销完成"); assert!(all[1].completed); assert!(!all[0].completed);
        assert!(call(&mut s, &mut all, "toggle", json!({"confirm":true,"context":old})).is_err());
        let context = s.ui["context"].clone();
        call(&mut s, &mut all, "toggle", json!({"confirm":true,"context":context})).unwrap();
        assert!(!all[1].completed); assert_eq!(all[1].revision, 3);
    }
    #[test]
    fn changed_deleted_and_different_selected_items_never_receive_an_old_confirmation() {
        let mut s = Reading::default(); let mut all = vec![item(1, "原内容"), item(2, "另一个")];
        call(&mut s, &mut all, "open", json!({})).unwrap(); let old = s.ui["context"].clone();
        all[0].title = "修改后的内容".into(); all[0].revision += 1;
        assert!(call(&mut s, &mut all, "toggle", json!({"confirm":true,"context":old})).is_err());
        all.remove(0);
        call(&mut s, &mut all, "status", json!({})).unwrap();
        assert!(!s.detail);
        call(&mut s, &mut all, "open", json!({})).unwrap();
        assert!(call(&mut s, &mut all, "toggle", json!({"confirm":true,"context":old})).is_err());
        assert!(!all[0].completed);
    }
    #[test]
    fn pagination_filtering_and_disabled_edges_cover_all_five_hundred_items() {
        for count in [0, 1, 5, 6, 20, 21, 500] {
            let mut s = Reading::default(); let mut all = (0..count).map(|n| item(n, "待办")).collect::<Vec<_>>();
            call(&mut s, &mut all, "status", json!({})).unwrap();
            assert!(!s.ui["operations"].as_array().unwrap().contains(&json!("previous")));
            while s.ui["operations"].as_array().unwrap().contains(&json!("next")) {
                call(&mut s, &mut all, "next", json!({})).unwrap();
            }
            assert_eq!(s.page, count.saturating_sub(1) / 5);
            let empty = call(&mut s, &mut all, "filter", json!({})).unwrap();
            assert_eq!(s.ui["operations"], json!(["filter"]));
            assert_eq!(empty["headline"], "还没有已完成事项");
        }
    }
    #[test]
    fn failed_atomic_write_never_changes_view_or_claims_success() {
        let mut s = Reading::default(); let mut all = vec![item(1, "测试")];
        call(&mut s, &mut all, "open", json!({})).unwrap(); let before = s.clone();
        let context = s.ui["context"].clone();
        let result = apply(&mut s, "toggle", &json!({"confirm":true,"context":context}), &mut |i| {
            if i.operation == "list" { Ok(all.clone()) } else { Err("无法写入".into()) }
        });
        assert!(result.is_err()); assert!(s == before);
    }
    #[test]
    fn production_store_persists_completion_and_rejects_concurrent_revision() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join("todos.json");
        let mut store = crate::todos::isolated_store(path.clone());
        let mut add = input("add"); add.title = Some("从设备完成这项".into()); store(add).unwrap();
        let mut s = Reading::default();
        apply(&mut s, "open", &json!({}), &mut store).unwrap();
        let context = s.ui["context"].clone();
        apply(&mut s, "toggle", &json!({"confirm":true,"context":context}), &mut store).unwrap();
        let mut reload = crate::todos::isolated_store(path);
        let saved = reload(input("list")).unwrap(); assert!(saved[0].completed);
        let stale = s.ui["context"].clone();
        let mut update = input("update"); update.id = Some(saved[0].id.clone()); update.revision = Some(saved[0].revision); update.title = Some("PC 改了内容".into());
        store(update).unwrap();
        assert!(apply(&mut s, "toggle", &json!({"confirm":true,"context":stale}), &mut store).is_err());
        assert!(store(input("list")).unwrap()[0].completed);
    }
    #[test]
    fn snapshot_export_uses_only_isolated_fixture_records() {
        let Some(output) = std::env::var_os("PET_TODO_UI_FIXTURES") else { return; };
        let dir = tempfile::tempdir().unwrap(); let mut store = crate::todos::isolated_store(dir.path().join("todos.json"));
        let mut s = Reading::default(); let mut fixtures = Vec::new();
        let mut capture = |name: &str, s: &mut Reading, op: &str, request: Value, store: &mut dyn FnMut(Input)->Result<Vec<Todo>,String>| {
            let values = apply(s, op, &request, &mut |i|store(i)).unwrap();
            let mut ui = s.ui.clone();
            ui["actions"] = ui["operations"].as_array().unwrap().iter().map(|o|json!(format!("host.{}",o.as_str().unwrap()))).collect::<Vec<_>>().into();
            ui.as_object_mut().unwrap().remove("operations");
            fixtures.push(json!({"name":name,"values":values,"ui":ui}));
        };
        capture("empty", &mut s, "status", json!({}), &mut store);
        for title in ["准备周会材料，列好本周需要确认的三个问题", "预约下周的体检", "买好周末露营需要的食物与饮水", "整理书桌和相机照片", "把项目进展发给合作伙伴", "给家人打个电话"] {
            let mut add=input("add");add.title=Some(title.into());store(add).unwrap();
        }
        capture("list", &mut s, "status", json!({}), &mut store);
        capture("page-two", &mut s, "next", json!({}), &mut store);
        capture("detail", &mut s, "row_a", json!({}), &mut store);
        let context=s.ui["context"].clone();
        capture("completed", &mut s, "toggle", json!({"confirm":true,"context":context}), &mut store);
        capture("back-list", &mut s, "open", json!({}), &mut store);
        let mut add=input("add");add.title=Some("一二三四五六七八九十".repeat(12));store(add).unwrap();
        capture("long-title-list", &mut s, "next", json!({}), &mut store);
        capture("long-title-one", &mut s, "row_a", json!({}), &mut store);
        capture("long-title-two", &mut s, "next", json!({}), &mut store);
        capture("completed-filter", &mut s, "filter", json!({}), &mut store);
        std::fs::write(output,serde_json::to_vec_pretty(&fixtures).unwrap()).unwrap();
    }
}
