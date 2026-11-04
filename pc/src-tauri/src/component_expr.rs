//! Bounded, side-effect-free JSON expressions. No scripts, filesystem or network.
//! Shared acceptance vectors are also executed by the authoring validator.
use serde_json::{json, Map, Value};
use std::collections::BTreeSet;

// Host data can contain epoch milliseconds and currency totals. Only the eight
// projected device variables retain the firmware's narrower +/- 1e9 limit.
const LIMIT: i64 = 9_007_199_254_740_991;
const FUEL: usize = 8192;
fn bad() -> String {
    "表达式超出类型、结构或计算预算".into()
}
fn safe_string(s: &str) -> bool {
    s.len() <= 512 && !s.chars().any(char::is_control)
}
pub fn bounded(v: &Value) -> bool {
    fn check(v: &Value, d: usize, n: &mut usize) -> bool {
        *n += 1;
        if d > 8 || *n > 1024 {
            return false;
        }
        match v {
            Value::Null | Value::Bool(_) => true,
            Value::Number(_) => v.as_i64().is_some_and(|n| (-LIMIT..=LIMIT).contains(&n)),
            Value::String(s) => safe_string(s),
            Value::Array(a) => a.len() <= 128 && a.iter().all(|v| check(v, d + 1, n)),
            Value::Object(o) => {
                o.len() <= 32
                    && o.iter()
                        .all(|(k, v)| !k.is_empty() && safe_string(k) && check(v, d + 1, n))
            }
        }
    }
    check(v, 0, &mut 0) && serde_json::to_vec(v).is_ok_and(|b| b.len() <= 8192)
}
fn pointer(s: &str) -> bool {
    if s.len() > 160 || !s.starts_with('/') || s.chars().any(char::is_control) {
        return false;
    }
    let mut c = s.chars();
    while let Some(v) = c.next() {
        if v == '~' && !matches!(c.next(), Some('0' | '1')) {
            return false;
        }
    }
    true
}
fn arity(op: &str, n: usize) -> bool {
    match op {
        "if" | "slice" | "clamp" => n == 3,
        "sub" | "div" | "mod" | "eq" | "ne" | "lt" | "lte" | "gt" | "gte" | "get" => n == 2,
        "not" | "len" | "str" | "sum" | "filter" | "map" | "sort" => n == 1,
        "add" | "mul" | "min" | "max" | "and" | "or" | "concat" | "coalesce" | "array" => {
            (1..=16).contains(&n)
        }
        _ => false,
    }
}
/// Returns explicitly referenced source aliases for acyclic scheduling.
pub fn validate(v: &Value, sources: &BTreeSet<String>) -> Result<BTreeSet<String>, String> {
    fn walk(
        v: &Value,
        known: &BTreeSet<String>,
        deps: &mut BTreeSet<String>,
        depth: usize,
        nodes: &mut usize,
        row: bool,
    ) -> Result<(), String> {
        *nodes += 1;
        if depth > 12 || *nodes > 192 {
            return Err(bad());
        }
        let Some(o) = v.as_object() else {
            return if v.is_null() || v.is_boolean() || v.is_number() || v.is_string() {
                if bounded(v) {
                    Ok(())
                } else {
                    Err(bad())
                }
            } else {
                Err(bad())
            };
        };
        if o.len() == 1 && o.contains_key("literal") {
            return if bounded(&o["literal"]) {
                Ok(())
            } else {
                Err(bad())
            };
        }
        if o.len() == 1 && o.contains_key("ref") {
            let p = o["ref"].as_str().filter(|p| pointer(p)).ok_or_else(bad)?;
            let parts: Vec<_> = p.split('/').collect();
            match parts.get(1).copied() {
                Some("sources") => {
                    let alias = parts
                        .get(2)
                        .filter(|n| known.contains(**n))
                        .ok_or_else(bad)?;
                    deps.insert((*alias).into());
                }
                Some("event") => {}
                Some("row" | "index") if row => {}
                _ => return Err(bad()),
            }
            return Ok(());
        }
        if o.len() == 1 && o.contains_key("object") {
            let fields = o["object"]
                .as_object()
                .filter(|m| m.len() <= 16)
                .ok_or_else(bad)?;
            for (k, value) in fields {
                if k.is_empty() || !safe_string(k) {
                    return Err(bad());
                }
                walk(value, known, deps, depth + 1, nodes, row)?;
            }
            return Ok(());
        }
        let op = o.get("op").and_then(Value::as_str).ok_or_else(bad)?;
        let a = o.get("args").and_then(Value::as_array).ok_or_else(bad)?;
        let iter = matches!(op, "filter" | "map" | "sort");
        if !arity(op, a.len())
            || o.len() != if iter { 3 } else { 2 }
            || o.keys()
                .any(|k| !matches!(k.as_str(), "op" | "args" | "value"))
            || iter != o.contains_key("value")
        {
            return Err(bad());
        }
        for v in a {
            walk(v, known, deps, depth + 1, nodes, row)?;
        }
        if iter {
            walk(&o["value"], known, deps, depth + 1, nodes, true)?;
        }
        Ok(())
    }
    let mut deps = BTreeSet::new();
    walk(v, sources, &mut deps, 0, &mut 0, false)?;
    Ok(deps)
}
fn integer(v: &Value) -> Result<i64, String> {
    v.as_i64()
        .filter(|n| (-LIMIT..=LIMIT).contains(n))
        .ok_or_else(bad)
}
fn number(v: i64) -> Result<Value, String> {
    if (-LIMIT..=LIMIT).contains(&v) {
        Ok(json!(v))
    } else {
        Err(bad())
    }
}
fn boolean(v: &Value) -> Result<bool, String> {
    v.as_bool().ok_or_else(bad)
}
fn compare(a: &Value, b: &Value) -> Result<std::cmp::Ordering, String> {
    match (a, b) {
        (Value::String(a), Value::String(b)) => Ok(a.cmp(b)),
        _ => Ok(integer(a)?.cmp(&integer(b)?)),
    }
}
fn run(
    v: &Value,
    ctx: &Value,
    row: Option<(&Value, usize)>,
    fuel: &mut usize,
) -> Result<Value, String> {
    *fuel = fuel.checked_sub(1).ok_or_else(bad)?;
    let Some(o) = v.as_object() else {
        return Ok(v.clone());
    };
    if let Some(v) = o.get("literal") {
        return Ok(v.clone());
    }
    if let Some(p) = o.get("ref").and_then(Value::as_str) {
        if p == "/index" {
            return row.map(|(_, i)| json!(i)).ok_or_else(bad);
        }
        if p == "/row" || p.starts_with("/row/") {
            return row
                .map(|(r, _)| {
                    if p == "/row" {
                        r.clone()
                    } else {
                        r.pointer(&p[4..]).cloned().unwrap_or(Value::Null)
                    }
                })
                .ok_or_else(bad);
        }
        return Ok(ctx.pointer(p).cloned().unwrap_or(Value::Null));
    }
    if let Some(fields) = o.get("object").and_then(Value::as_object) {
        let mut result = Map::new();
        for (k, e) in fields {
            result.insert(k.clone(), run(e, ctx, row, fuel)?);
        }
        return Ok(Value::Object(result));
    }
    let op = o["op"].as_str().ok_or_else(bad)?;
    let a = o["args"].as_array().ok_or_else(bad)?;
    if op == "if" {
        let cond = boolean(&run(&a[0], ctx, row, fuel)?)?;
        return run(&a[if cond { 1 } else { 2 }], ctx, row, fuel);
    }
    if op == "and" || op == "or" {
        for e in a {
            let b = boolean(&run(e, ctx, row, fuel)?)?;
            if b == (op == "or") {
                return Ok(json!(b));
            }
        }
        return Ok(json!(op == "and"));
    }
    if op == "coalesce" {
        for e in a {
            let value = run(e, ctx, row, fuel)?;
            if !value.is_null() {
                return Ok(value);
            }
        }
        return Ok(Value::Null);
    }
    let values: Vec<Value> = a
        .iter()
        .map(|v| run(v, ctx, row, fuel))
        .collect::<Result<_, _>>()?;
    let result = match op {
        "array" => Value::Array(values),
        "add" | "mul" => {
            let mut n = if op == "add" { 0i64 } else { 1 };
            for v in &values {
                let rhs = integer(v)?;
                n = if op == "add" {
                    n.checked_add(rhs)
                } else {
                    n.checked_mul(rhs)
                }
                .ok_or_else(bad)?;
                if !(-LIMIT..=LIMIT).contains(&n) {
                    return Err(bad());
                }
            }
            json!(n)
        }
        "sub" => number(
            integer(&values[0])?
                .checked_sub(integer(&values[1])?)
                .ok_or_else(bad)?,
        )?,
        "div" => number(
            integer(&values[0])?
                .checked_div(integer(&values[1])?)
                .ok_or_else(bad)?,
        )?,
        "mod" => number(
            integer(&values[0])?
                .checked_rem(integer(&values[1])?)
                .ok_or_else(bad)?,
        )?,
        "min" | "max" => {
            let n: Vec<_> = values.iter().map(integer).collect::<Result<_, _>>()?;
            json!(if op == "min" {
                n.iter().min()
            } else {
                n.iter().max()
            }
            .ok_or_else(bad)?)
        }
        "clamp" => {
            let (x, lo, hi) = (
                integer(&values[0])?,
                integer(&values[1])?,
                integer(&values[2])?,
            );
            if lo > hi {
                return Err(bad());
            }
            json!(x.clamp(lo, hi))
        }
        "eq" => json!(values[0] == values[1]),
        "ne" => json!(values[0] != values[1]),
        "lt" | "lte" | "gt" | "gte" => {
            let c = compare(&values[0], &values[1])?;
            json!(match op {
                "lt" => c.is_lt(),
                "lte" => c.is_le(),
                "gt" => c.is_gt(),
                _ => c.is_ge(),
            })
        }
        "not" => json!(!boolean(&values[0])?),
        "len" => json!(match &values[0] {
            Value::String(s) => s.chars().count(),
            Value::Array(a) => a.len(),
            Value::Object(o) => o.len(),
            _ => return Err(bad()),
        }),
        "str" => {
            if values[0].is_object() || values[0].is_array() {
                return Err(bad());
            }
            json!(values[0]
                .as_str()
                .map(str::to_owned)
                .unwrap_or_else(|| values[0].to_string()))
        }
        "concat" => {
            let mut s = String::new();
            for v in &values {
                s.push_str(v.as_str().ok_or_else(bad)?);
                if !safe_string(&s) {
                    return Err(bad());
                }
            }
            json!(s)
        }
        "get" => match &values[1] {
            Value::String(k) => values[0]
                .as_object()
                .and_then(|v| v.get(k))
                .cloned()
                .unwrap_or(Value::Null),
            _ => {
                let i = integer(&values[1])?;
                if i < 0 {
                    return Err(bad());
                }
                values[0]
                    .as_array()
                    .and_then(|a| a.get(i as usize))
                    .cloned()
                    .unwrap_or(Value::Null)
            }
        },
        "slice" => {
            let list = values[0].as_array().ok_or_else(bad)?;
            let (start, count) = (integer(&values[1])?, integer(&values[2])?);
            if start < 0 || count < 0 || count > 128 {
                return Err(bad());
            }
            json!(list
                .iter()
                .skip(start as usize)
                .take(count as usize)
                .cloned()
                .collect::<Vec<_>>())
        }
        "sum" => {
            let mut sum = 0i64;
            for v in values[0].as_array().ok_or_else(bad)? {
                sum = sum.checked_add(integer(v)?).ok_or_else(bad)?;
                if !(-LIMIT..=LIMIT).contains(&sum) {
                    return Err(bad());
                }
            }
            json!(sum)
        }
        "filter" | "map" | "sort" => {
            let list = values[0]
                .as_array()
                .filter(|a| a.len() <= 128)
                .ok_or_else(bad)?;
            let mut out = Vec::new();
            let mut keyed = Vec::new();
            for (i, r) in list.iter().enumerate() {
                let value = run(&o["value"], ctx, Some((r, i)), fuel)?;
                match op {
                    "filter" => {
                        if boolean(&value)? {
                            out.push(r.clone());
                        }
                    }
                    "map" => out.push(value),
                    _ => {
                        if !value.is_string() && integer(&value).is_err() {
                            return Err(bad());
                        }
                        keyed.push((value, i, r.clone()));
                    }
                }
            }
            if op == "sort" {
                if let Some(first) = keyed.first() {
                    if keyed.iter().any(|v| v.0.is_string() != first.0.is_string()) {
                        return Err(bad());
                    }
                }
                keyed.sort_by(|a, b| compare(&a.0, &b.0).unwrap().then(a.1.cmp(&b.1)));
                out = keyed.into_iter().map(|(_, _, v)| v).collect();
            }
            json!(out)
        }
        _ => return Err(bad()),
    };
    if bounded(&result) {
        Ok(result)
    } else {
        Err(bad())
    }
}
pub fn eval(v: &Value, ctx: &Value, sources: &BTreeSet<String>) -> Result<Value, String> {
    validate(v, sources)?;
    let result = run(v, ctx, None, &mut FUEL.clone())?;
    if bounded(&result) {
        Ok(result)
    } else {
        Err(bad())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn shared_expression_vectors() {
        let cases: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/expressions-v1.json"
        ))
        .unwrap();
        for c in cases.as_array().unwrap() {
            let known = c["sources"]
                .as_array()
                .unwrap()
                .iter()
                .map(|s| s.as_str().unwrap().to_string())
                .collect();
            let got = eval(&c["expression"], &c["context"], &known);
            if c["reject"] == true {
                assert!(got.is_err(), "{}", c["name"]);
            } else {
                assert_eq!(got.unwrap(), c["expected"], "{}", c["name"]);
            }
        }
    }
    #[test]
    fn iteration_fuel_and_shape_limits_are_enforced() {
        let known = BTreeSet::new();
        let items = json!({"literal":vec![0;128]});
        let inner = json!({"op":"sum","args":[{"op":"map","args":[items.clone()],"value":0}]});
        let expression = json!({"op":"map","args":[items],"value":inner});
        assert!(eval(&expression, &json!({}), &known).is_err());
        assert!(eval(&json!({"literal":vec![0;129]}), &json!({}), &known).is_err());
        let mut expression = json!(true);
        for _ in 0..14 {
            expression = json!({"op":"not","args":[expression]});
        }
        assert!(validate(&expression, &known).is_err());
    }
}
