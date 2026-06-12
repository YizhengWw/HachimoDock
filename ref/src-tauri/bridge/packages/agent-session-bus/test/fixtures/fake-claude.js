#!/usr/bin/env node
"use strict";

/**
 * Fake `claude` CLI used by ClaudeCodeAdapter tests. Behaves enough like
 * the real Claude Code CLI to drive ClaudeCodeAdapter end-to-end:
 *
 * - `claude --version`          → "1.0.0\n"
 * - `claude -p <text> --output-format stream-json --verbose`
 *      → emits a system/init frame with a session_id, a few token deltas,
 *        and a final result frame.
 * - `claude -p <text> --resume <sid> --output-format stream-json --verbose`
 *      → reuses the supplied session id.
 *
 * Behavior knobs via env vars:
 *   FAKE_CLAUDE_VERSION   override version string
 *   FAKE_CLAUDE_FAIL=1    exit nonzero with an error frame
 *   FAKE_CLAUDE_REPLY     override reply text (default: "改好了")
 */

const args = process.argv.slice(2);

if (args.length === 1 && args[0] === "--version") {
  const v = process.env.FAKE_CLAUDE_VERSION || "1.0.0";
  process.stdout.write(`${v}\n`);
  process.exit(0);
}

if (args.includes("-p")) {
  const reply = process.env.FAKE_CLAUDE_REPLY || "改好了";
  let sid = "fake-sid-" + Math.floor(Math.random() * 1e6).toString(36);
  const resumeIdx = args.indexOf("--resume");
  if (resumeIdx >= 0 && args[resumeIdx + 1]) sid = args[resumeIdx + 1];

  const emit = (obj) => process.stdout.write(JSON.stringify(obj) + "\n");

  emit({ type: "system", subtype: "init", session_id: sid });

  if (process.env.FAKE_CLAUDE_FAIL === "1") {
    emit({ type: "error", error: { type: "rate_limit", message: "fake fail" } });
    process.exit(1);
  }

  for (const ch of reply) {
    emit({ type: "content_block_delta", delta: { text: ch } });
  }
  emit({
    type: "result",
    session_id: sid,
    stop_reason: "end_turn",
    usage: { input_tokens: 5, output_tokens: reply.length },
  });
  process.exit(0);
}

process.stderr.write(`fake-claude: unsupported args: ${JSON.stringify(args)}\n`);
process.exit(2);
