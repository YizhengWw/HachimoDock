#!/usr/bin/env node
"use strict";

/**
 * Fake `codex` CLI used by CodexAdapter tests. Mirrors Codex CLI 0.118+
 * surface (which is what CodexAdapter targets in production).
 *
 *   codex --version
 *     → "codex-cli <FAKE_CODEX_VERSION || 0.118.0>\n"
 *
 *   codex exec [--json] [--skip-git-repo-check] [--sandbox <policy>] -- <prompt>
 *     → JSONL: thread.started → turn.started → item.completed{agent_message}
 *              → turn.completed
 *
 *   codex exec resume [--json] [--skip-git-repo-check] [--sandbox <policy>]
 *                     <SESSION_ID> -- <prompt>
 *     → same as above, but `thread_id` echoes <SESSION_ID>.
 *
 *   codex app-server --listen stdio://
 *     → tiny JSON-RPC app-server stub used for Desktop-thread continuation.
 *
 * Behavior knobs via env vars:
 *   FAKE_CODEX_VERSION    override version
 *   FAKE_CODEX_VERSION_STDERR=1
 *                         write --version output to stderr instead of stdout
 *   FAKE_CODEX_FAIL=1     emit turn.failed + exit nonzero
 *   FAKE_CODEX_UNSUPPORTED_MODEL_ON_RESUME=1
 *                         fail only `exec resume` like a Codex account/model mismatch
 *   FAKE_CODEX_REPLY      override reply text (default: "done")
 */

const args = process.argv.slice(2);

if (args.length === 1 && args[0] === "--version") {
  const v = process.env.FAKE_CODEX_VERSION || "0.118.0";
  const stream = process.env.FAKE_CODEX_VERSION_STDERR === "1" ? process.stderr : process.stdout;
  stream.write(`codex-cli ${v}\n`);
  process.exit(0);
}

if (args[0] === "app-server" && args[1] === "--listen" && args[2] === "stdio://") {
  let buffer = "";
  const reply = process.env.FAKE_CODEX_REPLY || "done";

  const write = (obj) => process.stdout.write(JSON.stringify(obj) + "\n");
  const handle = (frame) => {
    if (!frame || typeof frame !== "object") return;
    const id = frame.id;
    const method = typeof frame.method === "string" ? frame.method : "";
    if (method === "initialize") {
      write({ id, result: { protocolVersion: 1 } });
      return;
    }
    if (method === "thread/resume") {
      const threadId = frame.params?.threadId || "fake-app-thread";
      write({
        id,
        result: {
          thread: {
            id: threadId,
            model: "gpt-5.5",
            turns: [],
          },
        },
      });
      return;
    }
    if (method === "turn/start") {
      const threadId = frame.params?.threadId || "fake-app-thread";
      const turnId = "turn_fake_1";
      const itemId = "item_fake_1";
      const item = { id: itemId, type: "agentMessage", text: reply };
      write({
        id,
        result: {
          turn: { id: turnId, status: "inProgress", items: [] },
        },
      });
      if (process.env.FAKE_CODEX_APP_SERVER_FAIL === "1") {
        write({
          method: "turn/completed",
          params: {
            threadId,
            turn: {
              id: turnId,
              status: "failed",
              items: [],
              error: { message: "fake app-server fail" },
            },
          },
        });
        setTimeout(() => process.exit(1), 5);
        return;
      }
      write({
        method: "item/agentMessage/delta",
        params: { threadId, turnId, itemId, delta: reply },
      });
      write({
        method: "item/completed",
        params: { threadId, turnId, completedAtMs: Date.now(), item },
      });
      write({
        method: "turn/completed",
        params: {
          threadId,
          turn: {
            id: turnId,
            status: "completed",
            items: [item],
          },
        },
      });
      setTimeout(() => process.exit(0), 5);
      return;
    }
    write({
      id,
      error: { code: "UNKNOWN_METHOD", message: `unsupported method ${method}` },
    });
  };

  process.stdin.setEncoding("utf8");
  process.stdin.on("data", (chunk) => {
    buffer += chunk;
    let idx;
    while ((idx = buffer.indexOf("\n")) >= 0) {
      const line = buffer.slice(0, idx).trim();
      buffer = buffer.slice(idx + 1);
      if (!line) continue;
      try {
        handle(JSON.parse(line));
      } catch (error) {
        process.stderr.write(`fake-codex app-server parse error: ${error.message}\n`);
      }
    }
  });
  process.stdin.on("end", () => process.exit(0));
  return;
}

// We only support `exec` and `exec resume`.
if (args[0] === "exec") {
  // Detect resume subcommand: codex exec resume [...flags] <SID> -- <prompt>
  let sid = "";
  let isResume = false;
  let i = 1;
  if (args[i] === "resume") {
    isResume = true;
    i += 1;
    // The first non-flag, non-`--` token after `resume` is the SID.
    while (i < args.length) {
      const tok = args[i];
      if (tok === "--") break;
      if (tok.startsWith("--")) {
        // skip the flag and (when applicable) its value. We only know
        // about flags that take a value: `--sandbox`.
        if (tok === "--sandbox") { i += 2; continue; }
        i += 1;
        continue;
      }
      sid = tok;
      i += 1;
      break;
    }
  }
  if (!sid) sid = "fake-codex-" + Math.floor(Math.random() * 1e6).toString(36);

  const emit = (obj) => process.stdout.write(JSON.stringify(obj) + "\n");

  emit({ type: "thread.started", thread_id: sid });
  emit({ type: "turn.started" });

  if (isResume && process.env.FAKE_CODEX_UNSUPPORTED_MODEL_ON_RESUME === "1") {
    process.stderr.write("The 'gpt-5.3-codex' model is not supported when using Codex with a ChatGPT account.\n");
    process.exit(1);
  }

  if (process.env.FAKE_CODEX_FAIL === "1") {
    emit({
      type: "turn.failed",
      error: { code: "tool_error", message: "fake fail" },
    });
    process.exit(1);
  }

  const reply = process.env.FAKE_CODEX_REPLY || "done";
  emit({
    type: "item.completed",
    item: { id: "item_0", type: "agent_message", text: reply },
  });
  emit({
    type: "turn.completed",
    usage: { input_tokens: 1, output_tokens: 1 },
  });

  process.exit(0);
}

process.stderr.write(`fake-codex: unsupported args: ${JSON.stringify(args)}\n`);
process.exit(2);
