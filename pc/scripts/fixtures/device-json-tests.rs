// Shared actual-function tests for the desktop module and its offline harness.
// No transport, app initialization, credentials, user records or effects.
use super::*;

#[test]
fn device_file_parse_errors_are_not_silently_treated_as_missing_optional_values() {
    let deep="[".repeat(200)+"0"+&"]".repeat(200);
    for data in [b"{".to_vec(),vec![0xff],br#"{"v":"\ud800"}"#.to_vec(),deep.into_bytes()] {
        for path in ["component.json","negative-screen.json","buttons.json","runtime/widget.json"] {
            let files=HashMap::from([(path.into(),data.clone())]);let before=files.clone();let mut errors=vec![];
            validate_p4_device_json_strings(&files,&mut errors);
            assert!(!errors.is_empty(),"device file parse failure silently skipped: {path}");
            assert_eq!(files,before);
        }
        for path in ["pc/application.json","assets/synthetic.bin"] {
            let files=HashMap::from([(path.into(),data.clone())]);let mut errors=vec![];
            validate_p4_device_json_strings(&files,&mut errors);assert!(errors.is_empty());
        }
    }
}

#[test]
fn actual_device_depth_guard_matches_shared_bound_without_applying_to_private_pc_files() {
    let suite:serde_json::Value=serde_json::from_str(include_str!("../../skills/petui/contracts/device-json-depth-v1.json")).unwrap();
    assert_eq!(suite["maximumContainerDepth"],32);
    for case in suite["cases"].as_array().unwrap() {
        let result=serde_json::from_str::<serde_json::Value>(case["source"].as_str().unwrap());
        let expected=case["allowed"].as_bool().unwrap();
        assert_eq!(result.as_ref().is_ok_and(p4_json_depth_within),expected,"{}",case["name"]);
        if let Ok(value)=result {
            let before=value.clone();
            for path in ["component.json","negative-screen.json","buttons.json","runtime/widget.json"] {
                let files=HashMap::from([(path.into(),serde_json::to_vec(&value).unwrap())]);let mut errors=vec![];
                validate_p4_device_json_strings(&files,&mut errors);assert_eq!(errors.is_empty(),expected);
            }
            let files=HashMap::from([("pc/application.json".into(),serde_json::to_vec(&value).unwrap())]);let mut errors=vec![];
            validate_p4_device_json_strings(&files,&mut errors);assert!(errors.is_empty());assert_eq!(value,before);
        }
    }
}

#[test]
fn actual_serde_unicode_scalars_agree_with_device_text_contract() {
    let suite: serde_json::Value = serde_json::from_str(include_str!(
        "../../skills/petui/contracts/device-json-unicode-v1.json"
    )).unwrap();
    for case in suite["cases"].as_array().unwrap() {
        let result=serde_json::from_str::<serde_json::Value>(case["source"].as_str().unwrap());
        assert_eq!(result.as_ref().is_ok_and(p4_json_strings_no_nul),case["allowed"].as_bool().unwrap(),"{}",case["name"]);
    }
    for case in suite["byteCases"].as_array().unwrap() {
        let source=case["hex"].as_str().unwrap();
        let bytes:Vec<u8>=(0..source.len()).step_by(2).map(|i|u8::from_str_radix(&source[i..i+2],16).unwrap()).collect();
        let result=serde_json::from_slice::<serde_json::Value>(&bytes);
        assert_eq!(result.as_ref().is_ok_and(p4_json_strings_no_nul),case["allowed"].as_bool().unwrap(),"{}",case["name"]);
    }
}

#[test]
fn shared_vectors_and_private_pc_data_boundary() {
    let suite: serde_json::Value = serde_json::from_str(include_str!(
        "../../skills/petui/contracts/device-json-strings-v1.json"
    )).unwrap();
    for case in suite["cases"].as_array().unwrap() {
        let value = case["value"].clone();
        let before = value.clone();
        let expected = case["allowed"].as_bool().unwrap();
        assert_eq!(p4_json_strings_no_nul(&value), expected);
        assert_eq!(value, before);
        for path in ["component.json", "negative-screen.json", "buttons.json", "runtime/widget.json"] {
            let files = HashMap::from([(path.to_owned(), serde_json::to_vec(&value).unwrap())]);
            let mut errors = vec![];
            validate_p4_device_json_strings(&files, &mut errors);
            assert_eq!(errors.is_empty(), expected);
        }
        // Private PC document data and binary assets are not device C strings.
        let files = HashMap::from([("pc/application.json".to_owned(), serde_json::to_vec(&value).unwrap())]);
        let mut errors = vec![];
        validate_p4_device_json_strings(&files, &mut errors);
        assert!(errors.is_empty());
    }
}

#[test]
fn actual_variable_validator_refuses_nul_without_truncation() {
    for text in ["A\0B", "\0", "end\0"] {
        let variables = serde_json::json!({"label":{"type":"string","init":text}});
        let before = variables.clone();
        assert!(validate_p4_vars_object(variables.as_object().unwrap()).is_err());
        assert_eq!(variables, before);
    }
    let variables = serde_json::json!({"label":{"type":"string","init":r"A\u0000B"}});
    assert!(validate_p4_vars_object(variables.as_object().unwrap()).is_ok());
}
