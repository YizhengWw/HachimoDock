"""Compile and run the heap-free Wi-Fi link policy on the host.

Input: pet_p4_net_core.c and its assertion executable.
Output: regression checks for the Wi-Fi topic allowlists, credential rules, authentication
        message, lockout window and reconnect backoff, plus a cross-check that the firmware and
        desktop allowlists name the same business topics.
Position: pytest wrapper for the P4 Wi-Fi link policy.
Sync: update with pet_p4_net_core.c/.h, p4_net_core_test.c and pc/src-tauri/src/network_transport.rs.
"""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def test_net_core_policy_on_host():
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "p4-net-core-test"
        subprocess.run(
            [
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "main"),
                str(ROOT / "main" / "pet_p4_net_core.c"),
                str(ROOT / "tests" / "p4_net_core_test.c"),
                "-o", str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


def _strings(block: str):
    return set(re.findall(r'"([^"]+)"', block))


def test_firmware_and_desktop_allowlists_name_the_same_topics():
    firmware = (ROOT / "main" / "pet_p4_net_core.c").read_text()
    desktop = (ROOT.parent / "pc" / "src-tauri" / "src" / "network_transport.rs").read_text()
    fw_topics = _strings(re.search(r"k_inbound_topics\[\] = \{(.*?)\};", firmware, re.S).group(1))
    fw_prefixes = _strings(re.search(r"k_inbound_prefixes\[\] = \{(.*?)\};", firmware, re.S).group(1))
    pc_topics = _strings(re.search(r"const NETWORK_TOPICS: &\[&str\] = &\[(.*?)\];", desktop, re.S).group(1))
    pc_prefixes = _strings(re.search(r"const NETWORK_TOPIC_PREFIXES: &\[&str\] = &\[(.*?)\];", desktop, re.S).group(1))
    assert fw_topics == pc_topics
    assert fw_prefixes == pc_prefixes
