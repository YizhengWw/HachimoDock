"use strict";

const args = process.argv.slice(2);

if (args.includes("--version")) {
  process.stdout.write(`${process.env.FAKE_MIMO_VERSION || "0.1.6"}\n`);
  process.exit(0);
}

if (args[0] === "session" && args[1] === "list") {
  process.stdout.write(process.env.FAKE_MIMO_SESSIONS || "[]");
  process.stdout.write("\n");
  process.exit(0);
}

process.stderr.write(`unsupported fake mimo args: ${args.join(" ")}\n`);
process.exit(2);
