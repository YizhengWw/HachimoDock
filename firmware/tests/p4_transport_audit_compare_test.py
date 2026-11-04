import importlib.util
import json
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location("audit_compare", Path(__file__).resolve().parents[1] / "tools/compare_transport_audit.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def audit(body, **overrides):
    record = dict(seq=1, route=1, bodyBytes=len(body), fnv=MODULE.fnv(body),
                  hashAvailable=True, driverObserved=True, invalidReturn=False,
                  bodyAccepted=len(body), newlineAccepted=1, bodyDrain=0, newlineDrain=0)
    record.update(overrides)
    return dict(topic="debug/transport_audit_result", payload=dict(ok=True, active=False,
                session=1, seen=1, records=[record]))


def test_exact_raw_utf8_metadata_only_and_no_delivery_claim():
    body = '{"text":"私有文本"} '.encode()
    report = MODULE.compare(audit(body), body + b"\n", 1)
    record = report["records"][0]
    assert record["driverAcceptedComplete"] and record["driverDrainsSucceeded"]
    assert record["matchingRawLines"][0]["jsonObject"]
    assert report["deliveryConfirmed"] is False
    assert "私有文本" not in json.dumps(report, ensure_ascii=False)
    assert not MODULE.compare(audit(body), body.rstrip() + b"\n", 1)["records"][0]["matchingRawLines"]


def test_invalid_json_can_correlate_but_corruption_does_not():
    body = b'{"broken":}'
    result = MODULE.compare(audit(body), body + b"\n", 1)["records"][0]
    assert result["matchingRawLines"][0]["jsonObject"] is False
    assert not MODULE.compare(audit(body), body[:-2] + b"\n", 1)["records"][0]["matchingRawLines"]
    for body in (b'{"x":NaN}', b'{"x":Infinity}'):
        assert not MODULE.compare(audit(body), body + b'\n', 1)["records"][0]["matchingRawLines"][0]["jsonObject"]


def test_duplicate_missing_newline_short_write_and_unobserved_are_distinct():
    body = b'{"ok":true}'
    duplicate = MODULE.compare(audit(body), body + b"\n" + body, 1)["records"][0]
    assert duplicate["ambiguous"] and not duplicate["matchingRawLines"][1]["newlinePresent"]
    short = MODULE.compare(audit(body, newlineAccepted=0), body, 1)["records"][0]
    assert not short["driverAcceptedComplete"] and short["driverDrainsSucceeded"]
    native = MODULE.compare(audit(body, route=3, driverObserved=False), body, 3)["records"][0]
    assert not native["driverAcceptedComplete"] and not native["driverDrainsSucceeded"]


def test_bad_bounds_or_sequences_are_rejected():
    value = audit(b'{}')
    value["payload"]["records"][0]["seq"] = 2
    with pytest.raises(ValueError):
        MODULE.compare(value, b'{}\n', 1)
    with pytest.raises(ValueError):
        MODULE.compare(audit(b'{}'), b'{}', 0)
    with pytest.raises(ValueError):
        MODULE.compare(audit(b'{}'), b'\n' * MODULE.MAX_LINES, 1)
    oversized = b'x' * (MODULE.MAX_BODY_BYTES + 1)
    assert not MODULE.compare(audit(b'{}'), oversized, 1)["records"][0]["matchingRawLines"]
    value = audit(b'{}')
    value["payload"]["session"] = "private content must not be echoed"
    with pytest.raises(ValueError):
        MODULE.compare(value, b'{}\n', 1)


def test_repeated_sender_records_cannot_claim_one_received_line_twice():
    value = audit(b'{}')
    value["payload"]["seen"] = 2
    value["payload"]["records"].append(dict(value["payload"]["records"][0], seq=2))
    report = MODULE.compare(value, b'{}\n', 1)
    assert all(record["ambiguous"] and record["matchingLineCount"] == 1 for record in report["records"])
    repeated = MODULE.compare(audit(b'{}'), b'{}\n' * 100, 1)["records"][0]
    assert repeated["matchesTruncated"] and repeated["matchingLineCount"] == 100
    assert len(repeated["matchingRawLines"]) == 32


def test_post_driver_source_observation_is_optional_strict_and_not_delivery():
    body = b'{"ok":true}'
    legacy = MODULE.compare(audit(body), body+b'\n', 1)["records"][0]
    assert legacy["sourceFingerprintStable"] is None
    stable = MODULE.compare(audit(body, fnvAfter=MODULE.fnv(body), hashAfterAvailable=True), body+b'\n', 1)
    assert stable["records"][0]["sourceFingerprintStable"] is True
    assert not stable["deliveryConfirmed"]
    changed = MODULE.compare(audit(body, fnvAfter=MODULE.fnv(b'changed'), hashAfterAvailable=True), body+b'\n', 1)["records"][0]
    assert changed["sourceFingerprintStable"] is False and changed["driverAcceptedComplete"]
    unknown = MODULE.compare(audit(body, fnvAfter="00000000", hashAfterAvailable=False), body+b'\n', 1)["records"][0]
    assert unknown["sourceFingerprintStable"] is None
    for fields in ({"hashAfterAvailable":True},{"hashAfterAvailable":1},
                   {"fnvAfter":"private-content"},{"fnvAfter":0},{"fnvAfter":"1234567g"}):
        with pytest.raises(ValueError):
            MODULE.compare(audit(body, **fields), body+b'\n', 1)

def test_self_result_observation_has_distinct_scope_and_no_delivery_claim():
    body=b'{"topic":"debug/transport_audit_result","payload":{"ok":true}}'
    value=audit(body,fnvAfter=MODULE.fnv(body),hashAfterAvailable=True)
    value["topic"]="debug/transport_audit_reply_result"
    result=MODULE.compare(value,body+b'\n',1)
    assert result["observationScope"]=="audit-result-reply"
    assert result["records"][0]["sourceFingerprintStable"] is True
    assert not result["deliveryConfirmed"]
    assert MODULE.compare(audit(body),body+b'\n',1)["observationScope"]=="ordinary-frames"
    for topic in ("debug/transport_audit_started","debug/transport_audit_reply_read","unknown"):
        value["topic"]=topic
        with pytest.raises(ValueError):MODULE.compare(value,body+b'\n',1)
