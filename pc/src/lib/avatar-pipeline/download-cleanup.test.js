/** In-memory Web responses only; no native IPC, network, or user data. */
import test from "node:test";
import assert from "node:assert/strict";
import { downloadBinary } from "./http.js";

async function withResponse(response, run) {
  const original = globalThis.fetch;
  globalThis.fetch = async () => response;
  try { await run(); } finally { globalThis.fetch = original; }
}

for (const status of [200, 403]) {
  test(`early rejection cancels unread response body (HTTP ${status})`, async () => {
    let cancelled = false;
    const response = new Response(new ReadableStream({ cancel() { cancelled = true; } }),
      { status, headers: { "content-length": "1024" } });
    await withResponse(response, async () => {
      await assert.rejects(downloadBinary("https://fixture.invalid", undefined, { maxBytes: 8 }),
        status === 200 ? /字节上限/ : /HTTP 403/);
      assert.equal(cancelled, true);
      assert.equal(response.body.locked, false);
    });
  });
}

test("cleanup rejection does not replace the original size error", async () => {
  let cancellations = 0;
  const response = new Response(new ReadableStream({ cancel() { cancellations++; throw Error("cleanup failure"); } }),
    { headers: { "content-length": "1024" } });
  await withResponse(response, async () => {
    await assert.rejects(downloadBinary("https://fixture.invalid", undefined, { maxBytes: 8 }), /字节上限/);
    assert.equal(cancellations, 1);
  });
});

test("a streaming byte-limit failure cancels and releases the reader", async () => {
  let cancelled = false;
  const response = new Response(new ReadableStream({
    start(controller) { controller.enqueue(new Uint8Array(16)); },
    cancel() { cancelled = true; },
  }));
  await withResponse(response, async () => {
    await assert.rejects(downloadBinary("https://fixture.invalid", undefined, { maxBytes: 8 }), /字节上限/);
    assert.equal(cancelled, true);
    assert.equal(response.body.locked, false);
  });
});

test("successful bounded downloads preserve the complete bytes", async () => {
  await withResponse(new Response(new Uint8Array([1, 2, 3])), async () => {
    assert.deepEqual(await downloadBinary("https://fixture.invalid", undefined, { maxBytes: 8 }), new Uint8Array([1, 2, 3]));
  });
});

test("read failures release the response reader", async () => {
  const response = new Response(new ReadableStream({ pull(controller) { controller.error(Error("read failure")); } }));
  await withResponse(response, async () => {
    await assert.rejects(downloadBinary("https://fixture.invalid", undefined, { maxBytes: 8 }), /read failure/);
    assert.equal(response.body.locked, false);
  });
});
