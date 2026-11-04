"""Bundled PC catalog (search.catalog = pc/catalog.json): declaration, bounds and the city-radio package.
Source gates only; no network, device or audio."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc/skills/petui/scripts"))
from pc_extension import catalog_errors, extension_errors  # noqa: E402
from validate_generated_widget import DEFAULT_CAPABILITIES, validate_widget  # noqa: E402

PACKAGE = ROOT / "pc/builtin-clawpkgs/city-radio"


def load_extension():
    return json.loads((PACKAGE / "pc/extension.json").read_text(encoding="utf-8"))


def test_city_radio_package_is_offline_and_valid():
    e = load_extension()
    assert e["search"]["catalog"] == "pc/catalog.json" and e["search"]["endpoint"] == ""
    assert e["discovery"]["endpoints"] == []
    assert all("radio-browser" not in h for h in e["permissions"]["networkHosts"])
    assert extension_errors(e) == []
    assert validate_widget(PACKAGE, DEFAULT_CAPABILITIES) == []


def test_catalog_rows_are_mainland_only_and_bounded():
    raw = (PACKAGE / "pc/catalog.json").read_bytes()
    assert catalog_errors(raw) == []
    rows = json.loads(raw)
    assert 500 <= len(rows) <= 2000
    text = raw.decode("utf-8")
    for banned in ("香港", "澳门", "台湾", "CGTN", "CRI", "rfi.fr", "radio-browser", "tdm.com.mo"):
        assert banned not in text, banned
    assert any("中国之声" in r["name"] for r in rows)
    ids = [r["id"] for r in rows]
    assert len(ids) == len(set(ids))
    for r in rows:
        assert r["url"].startswith(("http://", "https://")) and r["available"] is True and r["region"]


def test_declaration_and_file_must_agree():
    e = load_extension()
    bad = json.loads(json.dumps(e)); bad["search"]["endpoint"] = "https://ipwho.is/search"
    assert extension_errors(bad)
    bad = json.loads(json.dumps(e)); bad["discovery"]["endpoints"] = ["https://ipwho.is/list"]
    assert extension_errors(bad)
    bad = json.loads(json.dumps(e)); bad["search"]["catalog"] = "pc/other.json"
    assert extension_errors(bad)
    offline_without_location = json.loads(json.dumps(e))
    offline_without_location["permissions"]["networkHosts"] = []
    offline_without_location["discovery"]["locationEndpoint"] = ""
    assert extension_errors(offline_without_location) == []
    assert catalog_errors(None) and catalog_errors(b"[]") and catalog_errors(b'[{"id":"a","nested":{"x":1}}]')
    assert catalog_errors(b'[{"bad key":"a"}]') and catalog_errors(b" " * (512 * 1024 + 1))
    assert catalog_errors('[{"id":"a","name":"台","url":"http://example.com/a","rank":1,"hls":false,"available":true}]'.encode()) == []
