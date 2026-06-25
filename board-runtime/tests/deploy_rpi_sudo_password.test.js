/*
 * [Input] board-runtime/scripts/deploy-rpi.sh sudo password wrapper.
 * [Output] Regression coverage that SUDO_PASSWORD auth preserves command stdin
 *          for sudo tee/cat deployment writes such as /etc/asound.conf.
 */

const assert = require("node:assert/strict");
const { readFileSync } = require("node:fs");
const { resolve } = require("node:path");
const test = require("node:test");

const script = readFileSync(resolve(__dirname, "../scripts/deploy-rpi.sh"), "utf8");

test("SUDO_PASSWORD path uses askpass instead of piping password into sudo stdin", () => {
  const remoteFunction = script.match(/remote\(\) \{[\s\S]*?\n\}/);
  assert.ok(remoteFunction, "expected remote() helper");
  assert.match(remoteFunction[0], /SUDO_ASKPASS/);
  assert.match(remoteFunction[0], /command sudo -A "\$@"/);
  assert.doesNotMatch(remoteFunction[0], /printf "%s\\n" "\$SUDO_PASSWORD" \| command sudo -S/);
});
