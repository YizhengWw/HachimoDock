use super::*;

fn target(host: &str, password: &str) -> SshStateFallbackTarget {
    SshStateFallbackTarget {
        host: host.into(), port: Some(2222), password: password.into(),
        root_dir: "/opt/board-runtime".into(),
    }
}

fn args(command: &Command) -> Vec<String> {
    command.get_args().map(|v| v.to_string_lossy().into_owned()).collect()
}

#[test]
fn ssh_target_rejects_option_and_shell_syntax() {
    for host in ["-oProxyCommand=fixture", "-Ffixture", "user@host;fixture", "$(fixture)", "host\n-oProxyCommand=fixture"] {
        assert!(normalize_widget_ssh_host(host).is_err());
        assert!(build_ssh_state_fallback_command(&target(host, "")).is_err());
    }
    assert!(build_ssh_state_fallback_command(&target("  ", "")).is_err());
}

#[test]
fn key_auth_keeps_persistent_host_identity_and_port() {
    let command = build_ssh_state_fallback_command(&target(" user@board.example ", "")).unwrap();
    let a = args(&command);
    assert_eq!(command.get_program(), "ssh");
    assert!(a.iter().any(|v| v == "BatchMode=yes"));
    assert!(a.iter().any(|v| v == "StrictHostKeyChecking=accept-new"));
    assert!(!a.iter().any(|v| v.starts_with("UserKnownHostsFile=")));
    assert!(a.windows(2).any(|v| v == ["-p", "2222"]));
    assert_eq!(a.last().unwrap(), "user@board.example");
    assert!(!command.get_envs().any(|(k, _)| k == "SSHPASS"));
}

#[test]
fn password_auth_is_preserved_without_password_argv_or_trust_bypass() {
    let command = build_ssh_state_fallback_command(&target("user@board.example", "synthetic-test-password")).unwrap();
    let a = args(&command);
    assert_eq!(command.get_program(), "sshpass");
    assert_eq!(&a[..2], ["-e", "ssh"]);
    assert!(a.iter().any(|v| v == "StrictHostKeyChecking=accept-new"));
    assert!(!a.iter().any(|v| v.contains("synthetic-test-password") || v == "StrictHostKeyChecking=no" || v.starts_with("UserKnownHostsFile=")));
    assert!(command.get_envs().any(|(k,v)| k == "SSHPASS" && v == Some(std::ffi::OsStr::new("synthetic-test-password"))));
}

#[test]
fn ordinary_dns_ip_and_ipv6_targets_remain_accepted() {
    for host in ["user@board.example", "192.0.2.10", "user@[2001:db8::1]", "user@board-local", "user@fe80::1%en0"] {
        assert_eq!(normalize_widget_ssh_host(host).unwrap().as_deref(), Some(host));
        assert!(build_ssh_state_fallback_command(&target(host, "")).is_ok());
    }
}
