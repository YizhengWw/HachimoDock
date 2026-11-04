"""Compare opt-in TX metadata with raw host bytes; never print frame contents.

This is correlation evidence, not delivery acknowledgement or a repair/retry
tool. Restrict --wire to the capture interval and --route to the physical link.
FNV collisions and repeated identical records prevent proof of causal matching.
"""
import argparse
import json
from pathlib import Path

MAX_WIRE_BYTES = 16 * 1024 * 1024
MAX_AUDIT_BYTES = 65536
MAX_LINES = 8192
MAX_BODY_BYTES = 32768


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return f"{value:08x}"


def reject_non_json_number(_value):
    raise ValueError("non-JSON numeric constant")


def compare(audit, wire, route):
    if len(wire) > MAX_WIRE_BYTES or route not in (1, 2, 3):
        raise ValueError("invalid capture bounds")
    if not isinstance(audit, dict) or audit.get("topic") not in (
            "debug/transport_audit_result", "debug/transport_audit_reply_result"):
        raise ValueError("not a transport audit response")
    payload = audit.get("payload")
    if not isinstance(payload, dict) or payload.get("ok") is not True or payload.get("active") is not False:
        raise ValueError("audit has not completed successfully")
    records = payload.get("records")
    seen = payload.get("seen")
    session = payload.get("session")
    if type(session) is not int or not 0 <= session <= 0xffffffff:
        raise ValueError("invalid audit session")
    if not isinstance(records, list) or len(records) > 16 or type(seen) is not int or not len(records) <= seen <= 64:
        raise ValueError("invalid audit record bounds")
    lines = []
    # Only LF is framing. Do not strip whitespace/CR, replace UTF-8 errors or
    # reserialize JSON before fingerprinting: that would alter the evidence.
    chunks = wire.split(b"\n", MAX_LINES)
    if len(chunks) > MAX_LINES:
        raise ValueError("capture has too many records")
    for index, body in enumerate(chunks):
        terminated = index < len(chunks) - 1
        if not body and not terminated:
            continue
        valid, fingerprint = None, None
        if len(body) <= MAX_BODY_BYTES:
            fingerprint = fnv(body)
            try:
                decoded = json.loads(body.decode("utf-8", errors="strict"),
                                     parse_constant=reject_non_json_number)
                valid = isinstance(decoded, dict)
            except (ValueError, UnicodeError, RecursionError):
                valid = False
        lines.append({"line": index + 1, "bytes": len(body), "fnv": fingerprint,
                      "jsonObject": valid, "newlinePresent": terminated})
    results = []
    for index, record in enumerate(records):
        if not isinstance(record, dict) or type(record.get("seq")) is not int or record.get("seq") != seen - len(records) + index + 1:
            raise ValueError("invalid audit sequence")
        if type(record.get("route")) is not int or record.get("route") not in (1, 2, 3):
            raise ValueError("invalid audit route")
        if record["route"] != route:
            continue
        size = record.get("bodyBytes")
        if type(size) is not int or not 0 <= size <= 0xffffffff:
            raise ValueError("invalid audit byte count")
        fingerprint = record.get("fnv")
        if not isinstance(fingerprint, str) or len(fingerprint) != 8 or any(c not in "0123456789abcdef" for c in fingerprint):
            raise ValueError("invalid audit fingerprint")
        for field in ("driverObserved", "hashAvailable", "invalidReturn"):
            if type(record.get(field)) is not bool:
                raise ValueError("invalid audit observation flag")
        after_available = record.get("hashAfterAvailable", False)
        after = record.get("fnvAfter")
        if type(after_available) is not bool or ("fnvAfter" in record and
                (not isinstance(after, str) or len(after) != 8 or any(c not in "0123456789abcdef" for c in after))):
            raise ValueError("invalid post-driver source observation")
        if after_available and after is None:
            raise ValueError("missing post-driver source fingerprint")
        source_stable = fingerprint == after if record["hashAvailable"] and after_available else None
        for field in ("bodyAccepted", "newlineAccepted", "bodyDrain", "newlineDrain"):
            if type(record.get(field)) is not int:
                raise ValueError("invalid driver observation")
        matches = [line for line in lines if record["hashAvailable"]
                   and line["bytes"] == size and line["fnv"] == fingerprint]
        duplicate_tx = sum(1 for other in records if isinstance(other, dict)
                           and other.get("route") == route and other.get("hashAvailable") is True
                           and other.get("bodyBytes") == size and other.get("fnv") == fingerprint) > 1
        observed = record["driverObserved"]
        driver_complete = observed and not record["invalidReturn"] and record["bodyAccepted"] == size and record["newlineAccepted"] == 1
        # Drain completion is distinct from acceptance, and neither is receipt.
        driver_drained = observed and record["bodyDrain"] == 0 and record["newlineDrain"] == 0
        results.append({"seq": record["seq"], "route": route,
                        "bodyBytes": size, "hashAvailable": record["hashAvailable"],
                        "driverObserved": observed, "driverAcceptedComplete": driver_complete,
                        "driverDrainsSucceeded": driver_drained,
                        "sourceFingerprintStable": source_stable,
                        "matchingRawLines": [{key: value for key, value in line.items() if key != "fnv"} for line in matches[:32]],
                        "matchingLineCount": len(matches), "matchesTruncated": len(matches) > 32,
                        "ambiguous": len(matches) > 1 or duplicate_tx})
    return {"session": session, "route": route,
            "observationScope": "audit-result-reply" if audit["topic"] == "debug/transport_audit_reply_result" else "ordinary-frames",
            "wireBytes": len(wire), "wireLines": len(lines),
            "seen": seen, "overwritten": seen - len(records), "records": results,
            "deliveryConfirmed": False,
            "interpretation": "Fingerprint correlation only; absence may be loss, corruption, an incomplete capture or ring eviction. No task is retried."}


def bounded_read(path, limit):
    with Path(path).open("rb") as stream:
        content = stream.read(limit + 1)
    if len(content) > limit:
        raise ValueError("capture exceeds size bound")
    return content


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--audit", required=True)
    parser.add_argument("--wire", required=True)
    parser.add_argument("--route", type=int, choices=(1, 2, 3), required=True,
                        help="1=UART, 2=USB Serial/JTAG, 3=native USB")
    args = parser.parse_args()
    try:
        report = compare(json.loads(bounded_read(args.audit, MAX_AUDIT_BYTES)),
                         bounded_read(args.wire, MAX_WIRE_BYTES), args.route)
    except (OSError, ValueError, RecursionError):
        # Do not echo private filesystem paths, JSON fragments or raw frames.
        parser.exit(2, "Could not validate local audit/capture files.\n")
    print(json.dumps(report, ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
