#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_rpi_env_wires_pairing_ap_commands() -> None:
    env = (ROOT / "board-runtime-rpi.env").read_text()

    assert "BOARD_RUNTIME_AP_UP_CMD=/opt/board-runtime/board-ap-up.sh" in env
    assert "BOARD_RUNTIME_AP_DOWN_CMD=/opt/board-runtime/board-ap-down.sh" in env
    assert "BOARD_RUNTIME_STA_APPLY_CMD=/opt/board-runtime/board-sta-apply.sh" in env
    assert "BOARD_RUNTIME_AP_COUNTRY=CN" in env


def test_rpi_deploy_installs_dhcp_server_for_pairing_ap() -> None:
    deploy = (ROOT / "scripts" / "deploy-rpi.sh").read_text()

    assert "dnsmasq" in deploy


def test_ap_up_releases_wlan0_from_networkmanager() -> None:
    ap_up = (ROOT / "board-ap-up.sh").read_text()

    assert 'nmcli dev disconnect "$IFACE"' in ap_up
    assert 'nmcli dev set "$IFACE" managed no' in ap_up
    assert "restore_networkmanager" in ap_up
    assert "country_code=$AP_COUNTRY" in ap_up


if __name__ == "__main__":
    test_rpi_env_wires_pairing_ap_commands()
    test_rpi_deploy_installs_dhcp_server_for_pairing_ap()
    test_ap_up_releases_wlan0_from_networkmanager()
