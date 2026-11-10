'use strict'

const test = require('node:test');
const assert = require('node:assert/strict');
const { STATES, messageEvent, reduce, isToolFailure } = require('../state-map');

test('messageEvent maps every documented Hermes row kind', () => {
  assert.deepEqual(messageEvent({ role: 'user', content: '跑一下' }), { state: 'working', event: 'user_message' });
  assert.deepEqual(
    messageEvent({ role: 'assistant', content: '', finish_reason: 'tool_calls' }),
    { state: 'thinking', event: 'tool_call_dispatch' },
  );
  assert.deepEqual(
    messageEvent({ role: 'assistant', content: '答案', finish_reason: 'stop' }),
    { state: 'attention', event: 'turn_end' },
  );
  assert.deepEqual(
    messageEvent({ role: 'assistant', content: '流式中', finish_reason: '' }),
    { state: 'speaking', event: 'assistant_streaming' },
  );
  assert.deepEqual(
    messageEvent({ role: 'tool', content: '{"output":"ok"}', tool_name: 'terminal' }),
    { state: 'working', event: 'tool_result', tool: 'terminal' },
  );
});

test('all 11 deskpet states are reachable from the mapping', () => {
  const reached = new Set([
    messageEvent({ role: 'user', content: 'x' }).state,
    messageEvent({ role: 'assistant', content: '', finish_reason: 'tool_calls' }).state,
    messageEvent({ role: 'assistant', content: 'x', finish_reason: '' }).state,
    messageEvent({ role: 'assistant', content: 'x', finish_reason: 'stop' }).state,
    messageEvent({ role: 'tool', content: '{"error":"boom"}' }).state,
    ...[
      reduce(null, {}).state,
      reduce(messageEvent({ role: 'user', content: 'x' }), { pendingClarify: true }).state,
      reduce(messageEvent({ role: 'user', content: 'x' }), { sessionEnded: true }).state,
      reduce(messageEvent({ role: 'user', content: 'x' }), { activeDelegation: true }).state,
      reduce(messageEvent({ role: 'user', content: 'x' }), { compressionActive: true }).state,
      reduce(messageEvent({ role: 'user', content: 'x' }), { activeWorker: true }).state,
    ],
  ]);
  assert.deepEqual([...reached].sort(), [...STATES].sort());
});

test('tool failure detection is content-based, not keyword-based', () => {
  assert.equal(isToolFailure('{"output":"all good"}'), false);
  assert.equal(isToolFailure('{"error":"ENOENT"}'), true);
  assert.equal(isToolFailure('{"is_error":true}'), true);
  assert.equal(isToolFailure('{"exit_code":1}'), true);
  assert.equal(isToolFailure('{"exit_code":0}'), false);
  assert.equal(isToolFailure('plain text failure mentioned in prose'), false);
  assert.equal(isToolFailure(''), false);
});

test('silent rows produce no event and reduce falls back to idle', () => {
  assert.equal(messageEvent({ role: 'session_meta', content: '{}' }), null);
  assert.equal(messageEvent({ role: 'assistant', content: '' }), null);
  assert.deepEqual(reduce(null, {}), { state: 'idle', event: 'no_activity' });
});

test('error outranks context elevations, clarify outranks session end', () => {
  const failed = messageEvent({ role: 'tool', content: '{"is_error":true}', tool_name: 'terminal' });
  assert.equal(reduce(failed, { sessionEnded: true, activeDelegation: true }).state, 'error');
  const working = messageEvent({ role: 'user', content: 'x' });
  assert.equal(reduce(working, { sessionEnded: true, pendingClarify: true }).state, 'notification');
  assert.equal(reduce(working, { sessionEnded: true }).state, 'sleeping');
});
