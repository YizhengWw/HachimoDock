//! Cross-language service/layout identity. Typed JSON framing avoids depending
//! on Python/JS/Rust's different floating-point spelling and exponent rules.
use super::*;
fn write(v:&Value,out:&mut Vec<u8>)->Result<()>{
    match v {
        Value::Null=>out.push(b'n'),
        Value::Bool(v)=>out.push(if *v{b't'}else{b'f'}),
        Value::Number(v)=>{
            // serde_json preserves JSON -0 as floating negative zero, while
            // Python parses that token as integer zero. One zero identity.
            if v.as_f64()==Some(0.0){out.extend_from_slice(b"i0;");}
            else if let Some(n)=v.as_i64(){out.push(b'i');out.extend_from_slice(n.to_string().as_bytes());out.push(b';');}
            else if let Some(n)=v.as_u64(){out.push(b'i');out.extend_from_slice(n.to_string().as_bytes());out.push(b';');}
            else{out.push(b'd');out.extend_from_slice(&v.as_f64().filter(|v|v.is_finite()).ok_or_else(fail)?.to_bits().to_be_bytes());}
        }
        Value::String(v)=>{out.push(b's');out.extend_from_slice(v.len().to_string().as_bytes());out.push(b':');out.extend_from_slice(v.as_bytes());}
        Value::Array(v)=>{out.push(b'a');out.extend_from_slice(v.len().to_string().as_bytes());out.push(b'[');for x in v{write(x,out)?;}out.push(b']');}
        Value::Object(v)=>{
            out.push(b'o');out.extend_from_slice(v.len().to_string().as_bytes());out.push(b'{');
            let mut keys:Vec<_>=v.keys().collect();keys.sort();for key in keys{write(&Value::String(key.clone()),out)?;write(&v[key],out)?;}out.push(b'}');
        }
    }Ok(())
}
pub fn package(declaration:&crate::app_services::Declaration,runtime:&Value)->Result<String>{
    let mut runtime=runtime.clone();runtime.as_object_mut().ok_or_else(fail)?.remove("host_contract");
    let declaration=json!({"schemaVersion":declaration.schema_version,"engine":declaration.engine,"service":declaration.service,"actions":declaration.actions,"content":declaration.content});
    let mut data=Vec::new();write(&json!([declaration,runtime]),&mut data)?;Ok(format!("{:x}",Sha256::digest(data)))
}
