"""Source boundary only: new outcome logic opens no real external connector."""
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2]

def test_external_outcomes_have_no_registered_transport_credentials_or_worker():
    source=(ROOT/'pc/src-tauri/src/component_external_outcome.rs').read_text().split('#[cfg(test)]')[0]
    for forbidden in ('reqwest','std::fs','File::','std::thread','std::env','tauri::command',
                      'impl Connector for','impl Journal for','impl Admission for','api_key','Authorization:'):
        assert forbidden not in source
    for file in ('component_composition.rs','clawpkg.rs','app_services.rs','component_extension.rs'):
        assert 'component_external_outcome' not in (ROOT/'pc/src-tauri/src'/file).read_text()
    assert 'component_external_outcome' not in (ROOT/'pc/src-tauri/permissions/local-app-commands.json').read_text()


def test_private_journal_is_native_only_without_connector_or_implicit_recovery():
    source=(ROOT/'pc/src-tauri/src/component_external_outcome/file_journal.rs').read_text().split('#[cfg(test)]\n#[derive')[0]
    for forbidden in ('reqwest', 'tauri::command', 'impl Connector for', 'impl Admission for',
                      'remove_file(', 'rename(', 'set_len(', 'create_dir', 'api_key', 'Authorization:'):
        assert forbidden not in source
    assert 'O_NOFOLLOW' in source and 'O_EXCL' in source and 'AT_SYMLINK_NOFOLLOW' in source
    assert 'LOCK_EX | libc::LOCK_NB' in source and 'F_FULLFSYNC' in source
    assert 'MAX_TRANSITIONS: u64 = 128' in source
    assert 'MAX_SLOTS: usize = 128' in source
    runner=(ROOT/'pc/scripts/test-component-workflows-offline.mjs').read_text()
    assert "'component_external_outcome/file_journal.rs'" in runner

def test_outcome_tests_are_in_actual_source_offline_harness():
    runner=(ROOT/'pc/scripts/test-component-workflows-offline.mjs').read_text()
    assert "'component_external_outcome'" in runner
    source=(ROOT/'pc/src-tauri/src/component_external_outcome.rs').read_text()
    assert 'pub(crate) fn submit_once' in source and 'pub(crate) fn reconcile_only' in source
    assert 'pub(crate) fn decode' in source and re.search(r'bytes\.len\(\)\s*>\s*2048',source)
