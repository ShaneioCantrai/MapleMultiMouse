// MapleMultiMouse OBS bridge — localhost-only, dependency-free Node.js 22+
// SPDX-License-Identifier: MIT
'use strict';

const http = require('node:http');
const crypto = require('node:crypto');

const DEFAULT_CONFIG = Object.freeze({
  websocketUrl: process.env.OBS_WEBSOCKET_URL || 'ws://127.0.0.1:4455',
  websocketPassword: process.env.OBS_WEBSOCKET_PASSWORD || '',
  httpPort: Number(process.env.MMM_BRIDGE_PORT || 4456),
  sceneA: process.env.MMM_SCENE_A || '',
  sceneB: process.env.MMM_SCENE_B || '',
  micInput: process.env.MMM_MIC_INPUT || '',
});

function obsAuthentication(password, salt, challenge) {
  const secret = crypto.createHash('sha256').update(password + salt).digest('base64');
  return crypto.createHash('sha256').update(secret + challenge).digest('base64');
}

class ObsConnection {
  constructor(url, password) {
    this.url = url;
    this.password = password;
    this.socket = null;
    this.ready = false;
    this.stopped = false;
    this.pending = new Map();
    this.reconnectTimer = null;
  }

  start() {
    if (this.stopped || this.socket) return;
    const ws = new WebSocket(this.url);
    this.socket = ws;
    ws.addEventListener('message', (event) => {
      let packet;
      try { packet = JSON.parse(String(event.data)); }
      catch { return; }
      this.onPacket(packet);
    });
    ws.addEventListener('error', (error) => {
      console.error('[OBS] WebSocket error:', error.message || 'connection failed');
    });
    ws.addEventListener('close', () => {
      this.ready = false;
      this.socket = null;
      for (const [id, pending] of this.pending) {
        clearTimeout(pending.timer);
        pending.reject(new Error('OBS disconnected'));
      }
      this.pending.clear();
      if (!this.stopped) this.reconnectTimer = setTimeout(() => this.start(), 2000);
    });
  }

  onPacket(packet) {
    const data = packet.d || {};
    if (packet.op === 0) { // Hello
      const identify = { rpcVersion: 1, eventSubscriptions: 0 };
      if (data.authentication) {
        identify.authentication = obsAuthentication(
          this.password, data.authentication.salt, data.authentication.challenge);
      }
      this.socket.send(JSON.stringify({ op: 1, d: identify }));
    } else if (packet.op === 2) { // Identified
      this.ready = true;
      console.log('[OBS] Connected and identified');
    } else if (packet.op === 7) { // RequestResponse
      const pending = this.pending.get(data.requestId);
      if (!pending) return;
      clearTimeout(pending.timer);
      this.pending.delete(data.requestId);
      if (data.requestStatus?.result === true) {
        pending.resolve(data.responseData || {});
      } else {
        pending.reject(new Error(data.requestStatus?.comment || 'OBS rejected request'));
      }
    }
  }

  request(requestType, requestData = {}) {
    if (!this.ready || !this.socket) return Promise.reject(new Error('OBS not connected'));
    const requestId = crypto.randomUUID();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error('OBS request timed out'));
      }, 4000);
      this.pending.set(requestId, { resolve, reject, timer });
      try {
        this.socket.send(JSON.stringify({
          op: 6, d: { requestType, requestId, requestData },
        }));
      } catch (err) {
        clearTimeout(timer);
        this.pending.delete(requestId);
        reject(err);
      }
    });
  }

  stop() {
    this.stopped = true;
    clearTimeout(this.reconnectTimer);
    if (this.socket) this.socket.close();
    this.ready = false;
  }
}

function createHttpServer(obs, config) {
  const actions = {
    scene_a: () => config.sceneA
      ? obs.request('SetCurrentProgramScene', { sceneName: config.sceneA })
      : Promise.reject(new Error('MMM_SCENE_A is not configured')),
    scene_b: () => config.sceneB
      ? obs.request('SetCurrentProgramScene', { sceneName: config.sceneB })
      : Promise.reject(new Error('MMM_SCENE_B is not configured')),
    toggle_mic: () => config.micInput
      ? obs.request('ToggleInputMute', { inputName: config.micInput })
      : Promise.reject(new Error('MMM_MIC_INPUT is not configured')),
  };
  function reply(res, code, obj) {
    res.writeHead(code, {
      'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'no-store',
      'X-Content-Type-Options': 'nosniff',
    });
    res.end(JSON.stringify(obj));
  }
  return http.createServer(async (req, res) => {
    // Loopback binding alone is not enough: browser-originated requests can
    // target localhost. Reject browser origins and all non-JSON mutations.
    if (req.headers.origin || req.method === 'OPTIONS') {
      return reply(res, 403, { ok: false, error: 'Browser origins are not allowed' });
    }
    if (req.method === 'GET' && req.url === '/health') {
      return reply(res, 200, {
        ok: true, connected: !!obs.ready,
        sceneA: config.sceneA, sceneB: config.sceneB, micInput: config.micInput,
      });
    }
    if (req.method !== 'POST' || req.url !== '/action') {
      return reply(res, 404, { ok: false, error: 'Unknown endpoint' });
    }
    if ((req.headers['content-type'] || '').split(';')[0].trim().toLowerCase()
        !== 'application/json') {
      return reply(res, 415, { ok: false, error: 'JSON required' });
    }
    let body = '';
    for await (const chunk of req) {
      body += chunk;
      if (body.length > 1024) return reply(res, 413, { ok: false, error: 'Too large' });
    }
    let action;
    try { action = JSON.parse(body).action; }
    catch { return reply(res, 400, { ok: false, error: 'Invalid JSON' }); }
    if (!Object.hasOwn(actions, action)) {
      return reply(res, 400, { ok: false, error: 'Action not allowed' });
    }
    if (!obs.ready) return reply(res, 503, { ok: false, error: 'OBS disconnected' });
    try {
      const result = await actions[action]();
      return reply(res, 200, { ok: true, result });
    } catch (err) {
      return reply(res, 502, { ok: false, error: err.message });
    }
  });
}

if (require.main === module) {
  if (!Number.isInteger(DEFAULT_CONFIG.httpPort) ||
      DEFAULT_CONFIG.httpPort < 1024 || DEFAULT_CONFIG.httpPort > 65535) {
    throw new Error('MMM_BRIDGE_PORT must be between 1024 and 65535');
  }
  const obs = new ObsConnection(
    DEFAULT_CONFIG.websocketUrl, DEFAULT_CONFIG.websocketPassword);
  const server = createHttpServer(obs, DEFAULT_CONFIG);
  server.listen(DEFAULT_CONFIG.httpPort, '127.0.0.1', () => {
    console.log('[Bridge] Listening only on 127.0.0.1:' + DEFAULT_CONFIG.httpPort);
    obs.start();
  });
  function stop() {
    obs.stop();
    server.close();
  }
  process.on('SIGTERM', stop);
  process.on('SIGINT', stop);
}

module.exports = { ObsConnection, obsAuthentication, createHttpServer, DEFAULT_CONFIG };
