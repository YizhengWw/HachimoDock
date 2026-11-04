//! Canonical (key-sorted) JSON for hashing and identities.
//!
//! `serde_json` keeps object keys in declaration order (feature `preserve_order`) so
//! that packages reach the device exactly as authored. Hashes, pack ids and cache
//! keys must not depend on authoring order, so they serialize a key-sorted copy —
//! the same form `stableJson` produces on the JavaScript side.
use serde::Serialize;
use serde_json::Value;

pub(crate) fn canonical(value: &Value) -> Value {
    match value {
        Value::Object(map) => {
            let mut keys: Vec<&String> = map.keys().collect();
            keys.sort();
            let mut sorted = serde_json::Map::with_capacity(map.len());
            for key in keys {
                sorted.insert(key.clone(), canonical(&map[key]));
            }
            Value::Object(sorted)
        }
        Value::Array(items) => Value::Array(items.iter().map(canonical).collect()),
        other => other.clone(),
    }
}

pub(crate) fn canonical_vec<T: Serialize>(value: &T) -> Result<Vec<u8>, serde_json::Error> {
    serde_json::to_vec(&canonical(&serde_json::to_value(value)?))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn canonical_sorts_keys_recursively_and_keeps_arrays() {
        let value: Value = serde_json::from_str(r#"{"b":{"z":1,"a":[{"y":2,"x":1}]},"a":0}"#).unwrap();
        assert_eq!(serde_json::to_string(&canonical(&value)).unwrap(), r#"{"a":0,"b":{"a":[{"x":1,"y":2}],"z":1}}"#);
        assert_eq!(serde_json::to_string(&value).unwrap(), r#"{"b":{"z":1,"a":[{"y":2,"x":1}]},"a":0}"#);
        assert_eq!(canonical_vec(&value).unwrap(), serde_json::to_vec(&canonical(&value)).unwrap());
    }
}
