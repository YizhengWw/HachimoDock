"""Opt-in real Codex failure probe through the running desktop Bridge.

One-shot CLI configuration targets an owned loopback HTTP503 fixture. It
preserves global config, persists a real rollout and never logs request bodies.
No mock session/state is injected into the Bridge or device.
"""
import argparse
import http.server
import json
import pathlib
import shutil
import subprocess
import threading
import time
import urllib.parse
import urllib.request


def run(args):
    if urllib.parse.urlparse(args.bridge).hostname not in ["localhost", "127.0.0.1", "::1"]:
        raise ValueError("The failure probe only supports a loopback Bridge")
    args.output.mkdir(parents=True, exist_ok=True)
    class Failure(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            remaining = int(self.headers.get("Content-Length", "0"))
            while remaining:
                block = self.rfile.read(min(65536, remaining))
                if not block:
                    break
                remaining -= len(block)
            time.sleep(args.response_delay)
            body = b'{"error":{"message":"StopWatch E2E controlled failure","type":"server_error","code":"e2e_unavailable"}}'
            self.send_response(503)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Failure)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    command = [args.codex, "exec", "--ignore-user-config", "--sandbox", "read-only",
               "--json", "--skip-git-repo-check", "-C", "/tmp",
               "-c", 'model_provider="stopwatch_e2e"',
               "-c", 'model_providers.stopwatch_e2e.name="StopWatch local error fixture"',
               "-c", 'model_providers.stopwatch_e2e.base_url="http://127.0.0.1:%d/v1"' % server.server_port,
               "-c", 'model_providers.stopwatch_e2e.wire_api="responses"',
               "-c", "model_providers.stopwatch_e2e.requires_openai_auth=false",
               "-c", "model_providers.stopwatch_e2e.supports_websockets=false",
               "-c", "model_providers.stopwatch_e2e.request_max_retries=0",
               "-c", "model_providers.stopwatch_e2e.stream_max_retries=0",
               "-c", "mcp_servers={}",
               "StopWatch 联调错误分支：这是受控失败验证，请只回复 STOPWATCH_ERROR_CASE。"]
    session = None
    observations = []
    stopped = threading.Event()

    def observe():
        previous = None
        while not stopped.wait(0.4):
            if not session:
                continue
            try:
                data = json.load(urllib.request.urlopen(args.bridge.rstrip("/") +
                    "/agent/sessions?agentId=codex&limit=50", timeout=2))
                row = next((x for x in data.get("sessions", []) if x.get("id") == session), None)
                if row and row.get("state") != previous:
                    item = {"time": time.time(), **{k: row.get(k) for k in
                            ["id", "name", "state", "lastModified"]}}
                    observations.append(item)
                    previous = row.get("state")
                    print(json.dumps(item, ensure_ascii=False), flush=True)
            except Exception:
                pass

    child = None
    timeout = None
    with (args.output / "codex.jsonl").open("w") as output, \
            (args.output / "stderr.log").open("w") as errors:
        observer = threading.Thread(target=observe, daemon=True)
        observer.start()
        try:
            child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors,
                                     stdin=subprocess.DEVNULL, text=True)
            timeout = threading.Timer(75, lambda: child.terminate() if child.poll() is None else None)
            timeout.daemon = True
            timeout.start()
            for line in child.stdout:
                output.write(line)
                output.flush()
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                if event.get("type") == "thread.started":
                    session = event.get("thread_id")
                    print("Real Codex test session: " + session, flush=True)
            code = child.wait(timeout=60)
            time.sleep(4)
        finally:
            if timeout:
                timeout.cancel()
            if child and child.poll() is None:
                child.terminate()
            stopped.set()
            observer.join(timeout=3)
            server.shutdown()
    passed = code != 0 and any(x["state"] in ["thinking", "working"] for x in observations) \
        and any(x["state"] == "error" for x in observations)
    report = {"sessionId": session, "exitCode": code, "passed": passed,
              "fixture": "owned loopback HTTP503; one-shot CLI config",
              "observations": observations}
    (args.output / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--codex", default=shutil.which("codex"))
    parser.add_argument("--bridge", default="http://127.0.0.1:8181")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--response-delay", type=int, default=12,
                        help="Seconds before the local 503, 1–45 (allows manual device selection)")
    args = parser.parse_args()
    if not args.codex:
        parser.error("Codex CLI is required")
    if not 1 <= args.response_delay <= 45:
        parser.error("--response-delay must be between 1 and 45 seconds")
    raise SystemExit(run(args))
