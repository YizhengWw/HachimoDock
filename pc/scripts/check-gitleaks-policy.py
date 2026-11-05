"""Exercise the real scanner's narrowly scoped keyboard-alphabet exception."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scanner", required=True)
    args = parser.parse_args()
    scanner = str(Path(args.scanner).resolve())
    config = Path(__file__).resolve().parents[2] / ".gitleaks.toml"
    alphabet = "0123456789.-"
    synthetic = hashlib.sha256(b"scanner-negative-fixture-not-a-service-key").hexdigest()
    target = "pc/skills/petui/scripts/app_form_v3.py"
    cases = [
        (target, "NUMERIC_KEYS=" + repr(alphabet), 0),
        (target, "NUMERIC_KEYS=" + repr(synthetic), 1),
        ("other.py", "NUMERIC_KEYS=" + repr(alphabet), 1),
        (target, "NUMERIC_KEYS=" + repr(alphabet) + "\napi_key=" + repr(synthetic), 1),
    ]
    for index, (name, text, expected) in enumerate(cases):
        with tempfile.TemporaryDirectory(prefix="scanner-policy-") as directory:
            root = Path(directory)
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text + "\n", encoding="utf-8")
            result = subprocess.run(
                [scanner, "dir", ".", "--config", str(config), "--redact=100", "--no-banner"],
                cwd=root, capture_output=True, text=True, timeout=30,
            )
            if result.returncode != expected:
                raise SystemExit(f"Scanner policy case {index + 1} failed (exit {result.returncode}, expected {expected})")
    print("Scanner policy: 4 cases passed; unrelated and changed values remain detectable.")


if __name__ == "__main__":
    main()
