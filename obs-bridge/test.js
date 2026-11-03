// SPDX-License-Identifier: MIT
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { ObsConnection, obsAuthentication, createHttpServer } = require('./server');

test('OBS v5 double-SHA256 authentication', () => {
  assert.equal(obsAuthentication('password', 'salt', 'challenge'),
               'zTM5ki6L2vVvBQiTG9ckH1Lh64AbnCf6XZ226UmnkIA=');
});

async function withServer(ready, fn) {
  const calls = [];
  const obs = {
    ready,
    request: async (method, data) => { calls.push({ method, data }); return {}; },
  };
  const cfg = { sceneA: 'Game', sceneB: 'BRB', micInput: 'Mic/Aux' };
  const server = createHttpServer(obs, cfg);
  await new Promise(ok => server.listen(0, '127.0.0.1', ok));
  try { await fn('http://127.0.0.1:' + server.address().port, calls); }
  finally { await new Promise(ok => server.close(ok)); }
}

function post(base, action, headers = {}) {
  return fetch(base + '/action', {
    method: 'POST', headers: { 'Content-Type': 'application/json', ...headers },
    body: JSON.stringify({ action }),
  });
}

test('health reports OBS connection and configured labels', async () => {
  await withServer(false, async base => {
    const response = await fetch(base + '/health');
    assert.equal(response.status, 200);
    assert.deepEqual(await response.json(), {
      ok: true, connected: false, sceneA: 'Game',
      sceneB: 'BRB', micInput: 'Mic/Aux',
    });
  });
});

test('browser Origin, wrong content type, bad action, and offline OBS denied', async () => {
  await withServer(false, async base => {
    assert.equal((await post(base, 'scene_a', { Origin: 'https://evil.example' })).status, 403);
    assert.equal((await fetch(base + '/action', {
      method: 'POST', headers: { 'Content-Type': 'text/plain' },
      body: '{"action":"scene_a"}',
    })).status, 415);
    assert.equal((await post(base, 'start_stream')).status, 400);
    assert.equal((await post(base, 'scene_a')).status, 503);
  });
});

test('allowlisted actions use exact OBS methods, never foreground input', async () => {
  await withServer(true, async (base, calls) => {
    for (const action of ['scene_a', 'scene_b', 'toggle_mic'])
      assert.equal((await post(base, action)).status, 200);
    assert.deepEqual(calls, [
      { method: 'SetCurrentProgramScene', data: { sceneName: 'Game' } },
      { method: 'SetCurrentProgramScene', data: { sceneName: 'BRB' } },
      { method: 'ToggleInputMute', data: { inputName: 'Mic/Aux' } },
    ]);
  });
});

test('client sends Identify then resolves only matching OBS request response', async () => {
  const sent = [];
  const client = new ObsConnection('ws://127.0.0.1:4455', 'pw');
  client.socket = { send: msg => sent.push(JSON.parse(msg)), close() {} };
  client.onPacket({ op: 0, d: {
    authentication: { salt: 'salt', challenge: 'challenge' },
  } });
  assert.equal(sent[0].op, 1);
  assert.equal(sent[0].d.authentication, obsAuthentication('pw', 'salt', 'challenge'));
  client.onPacket({ op: 2, d: {} });
  const waiting = client.request('GetVersion');
  const requestId = sent[1].d.requestId;
  client.onPacket({ op: 7, d: {
    requestId, requestStatus: { result: true }, responseData: { name: 'OBS' },
  } });
  assert.deepEqual(await waiting, { name: 'OBS' });
  client.stop();
});
