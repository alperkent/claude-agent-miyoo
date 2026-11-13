/*
 * Claude Agent relay for Miyoo Mini handhelds.
 *
 * - Each Miyoo opens a WebSocket to its own Durable Object (DeviceRoom) and waits.
 * - Claude reaches the Miyoo through the MCP endpoint /mcp, signed in with OAuth.
 *   Signing in means typing the pairing code shown on the Miyoo's screen.
 * - The relay keeps no conversation content: tool calls and results only pass through.
 */
import { OAuthProvider } from '@cloudflare/workers-oauth-provider';
import { DurableObject } from 'cloudflare:workers';

const VERSION = '0.4.0';
const SCOPE = 'miyoo';
const PAIR_CODE_TTL_MS = 10 * 60 * 1000;
const MAX_TOOL_WAIT_MS = 720 * 1000;

const SERVER_INSTRUCTIONS = `These tools control a Miyoo Mini handheld (Linux, ARM, OnionOS or RedOnion) owned by the user you are talking to.
- Start with device_info and a screenshot to see the state of the device.
- The user may not be technical: explain in plain words what you are about to change before changing system files, and say what you changed afterwards.
- Check your work: take a screenshot after pressing buttons or launching something.
- Do not press POWER unless the user asks; it can put the device to sleep or turn it off, and then you lose the connection.
- SD card root is /mnt/SDCARD. OnionOS system files live in /mnt/SDCARD/.tmp_update. Ports are in /mnt/SDCARD/Roms/PORTS, apps in /mnt/SDCARD/App.
- write_file keeps a backup of every file it replaces, and delete_path moves things to /mnt/SDCARD/App/ClaudeAgent/backups instead of erasing them: prefer delete_path to rm.
- The person holding the Miyoo is asked on its screen the first time each kind of action is used (see the screen, read files, change files, press buttons, run commands). If a permission is refused, explain what you needed it for and do not try again unless the user asks.
- The user may be looking at the Miyoo rather than at this chat: after each answer, call show_message with a short plain-text version of it (what you found, what you did, what they should do next).
- Launching games or leaving the app needs background_mode, which the user confirms on the device.
- If developer tools are on (download_file, input_record, input_play, view_image, job_status, run_app, perf_monitor, screen_capture, watch_file, snapshot): use download_file for files bigger than a few hundred KB (for example from a GitHub release) instead of write_file; run_app to open a game or app and see how it ends (crash, output, core dump); input_record / input_play to repeat the same button presses with exact timing (start each run from the same save state); perf_monitor for CPU, clock and memory; snapshot before risky experiments. Long tasks continue as jobs: follow them with job_status. Work on your own as far as the user's request and the permissions allow. If the user asks for these and they are missing, they can turn them on in the app: START > Developer options. New tools appear in new chats.
- Do everything on the device yourself with these tools (run_command runs scripts on the Miyoo). Never ask the user to install programs on a computer or to use a terminal. If a step truly needs a computer (for example copying a large game to the SD card), say that it is an advanced step and explain it in simple numbered steps.`;

/* ------------------------------------------------------------ helpers */

function randomHex(bytes) {
  const a = crypto.getRandomValues(new Uint8Array(bytes));
  return [...a].map((b) => b.toString(16).padStart(2, '0')).join('');
}

async function sha256Hex(text) {
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

function timingSafeEqualHex(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== b.length) return false;
  let r = 0;
  for (let i = 0; i < a.length; i++) r |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return r === 0;
}

function json(data, status = 200) {
  return new Response(JSON.stringify(data), { status, headers: { 'Content-Type': 'application/json' } });
}

function html(body, status = 200, headers = new Headers()) {
  headers.set('Content-Type', 'text/html; charset=utf-8');
  headers.set('X-Frame-Options', 'DENY');
  // no form-action: browsers apply it to the redirect back to the client after the form, too
  if (!headers.has('Content-Security-Policy')) headers.set('Content-Security-Policy', "default-src 'none'; style-src 'unsafe-inline'");
  return new Response(body, { status, headers });
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

const deviceStub = (env, deviceId) => env.DEVICE.get(env.DEVICE.idFromName(deviceId));
const pairingStub = (env) => env.PAIRING.get(env.PAIRING.idFromName('index'));

/* --------------------------------------------------- DeviceRoom (one per Miyoo) */

export class DeviceRoom extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.pending = new Map(); // call id -> {resolve, timer}
    this.nextId = 1;
    // keep-alive pings are answered without waking the object
    ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair('{"type":"ping"}', '{"type":"pong"}'));
  }

  async init(secretHash, deviceId) {
    if (await this.ctx.storage.get('secret')) return false;
    await this.ctx.storage.put({ secret: secretHash, deviceId, gen: 1, created: Date.now() });
    return true;
  }

  socket() {
    const s = this.ctx.getWebSockets('device');
    return s.length ? s[s.length - 1] : null;
  }

  async status() {
    const info = (await this.ctx.storage.get('info')) || null;
    return { online: !!this.socket(), info, lastSeen: (await this.ctx.storage.get('lastSeen')) || null };
  }

  async generation() {
    return (await this.ctx.storage.get('gen')) || 0;
  }

  async fetch(request) {
    // WebSocket from the Miyoo; the Worker already checked the Upgrade header
    const stored = await this.ctx.storage.get('secret');
    const auth = request.headers.get('Authorization') || '';
    const secret = auth.startsWith('Bearer ') ? auth.slice(7) : '';
    if (!stored || !timingSafeEqualHex(await sha256Hex(secret), stored)) {
      return new Response('unknown device or wrong secret', { status: 403 });
    }
    for (const old of this.ctx.getWebSockets('device')) {
      try {
        old.close(4000, 'replaced by a new connection');
      } catch {}
    }
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1], ['device']);
    await this.ctx.storage.put('lastSeen', Date.now());
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  async webSocketMessage(ws, message) {
    let msg;
    try {
      msg = JSON.parse(typeof message === 'string' ? message : new TextDecoder().decode(message));
    } catch {
      return;
    }
    if (msg.type === 'hello') {
      await this.ctx.storage.put({ tools: msg.tools || [], info: msg.info || null, lastSeen: Date.now() });
      ws.send(JSON.stringify({ type: 'welcome', server: VERSION }));
    } else if (msg.type === 'tools') {
      // the person turned developer tools on or off in the app
      await this.ctx.storage.put('tools', msg.tools || []);
    } else if (msg.type === 'result') {
      const p = this.pending.get(msg.id);
      if (p) {
        clearTimeout(p.timer);
        this.pending.delete(msg.id);
        p.resolve(msg.result);
      }
    } else if (msg.type === 'pair_request') {
      const code = await pairingStub(this.env).create(await this.ctx.storage.get('deviceId'));
      ws.send(JSON.stringify({ type: 'pair_code', code, expires_in: PAIR_CODE_TTL_MS / 1000 }));
    } else if (msg.type === 'unpair_all') {
      // every token issued before this stops working
      const gen = (await this.generation()) + 1;
      await this.ctx.storage.put('gen', gen);
      ws.send(JSON.stringify({ type: 'unpaired' }));
    }
  }

  async webSocketClose(ws, code) {
    try {
      ws.close(code, 'closing');
    } catch {}
  }

  async notifyPaired(clientName) {
    const ws = this.socket();
    if (ws) ws.send(JSON.stringify({ type: 'paired', client: clientName || 'Claude' }));
  }

  async tools(gen) {
    if (gen !== (await this.generation())) return { revoked: true };
    return { tools: (await this.ctx.storage.get('tools')) || [], online: !!this.socket() };
  }

  async call(gen, tool, args, waitMs) {
    if (gen !== (await this.generation())) return { revoked: true };
    const ws = this.socket();
    if (!ws) return { offline: true };
    const id = this.nextId++;
    const result = await new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        resolve({
          content: [{ type: 'text', text: `The device did not answer within ${Math.round(waitMs / 1000)} s.` }],
          isError: true,
        });
      }, waitMs);
      this.pending.set(id, { resolve, timer });
      try {
        ws.send(JSON.stringify({ type: 'call', id, tool, args: args || {} }));
      } catch (e) {
        clearTimeout(timer);
        this.pending.delete(id);
        resolve({ content: [{ type: 'text', text: 'The connection to the device dropped.' }], isError: true });
      }
    });
    await this.ctx.storage.put('lastSeen', Date.now());
    return { result };
  }
}

/* ------------------------------------------- PairingIndex (one for the whole relay) */

export class PairingIndex extends DurableObject {
  async create(deviceId) {
    const now = Date.now();
    // drop expired codes
    const all = await this.ctx.storage.list({ prefix: 'code:' });
    const stale = [...all].filter(([, v]) => v.expires < now).map(([k]) => k);
    if (stale.length) await this.ctx.storage.delete(stale);
    // one active code per device
    for (const [k, v] of all) if (v.deviceId === deviceId) await this.ctx.storage.delete(k);
    let code;
    do {
      code = String(100000 + (crypto.getRandomValues(new Uint32Array(1))[0] % 900000));
    } while (await this.ctx.storage.get('code:' + code));
    await this.ctx.storage.put('code:' + code, { deviceId, expires: now + PAIR_CODE_TTL_MS });
    return code;
  }

  // Returns the device for a code, or an error. The code is used up by consume(),
  // only once the sign-in went through.
  async redeem(code, attemptKey) {
    const now = Date.now();
    // brute-force brakes: 5 wrong codes per sign-in page, 60 wrong codes per minute for everyone
    const tries = (await this.ctx.storage.get('tries:' + attemptKey)) || 0;
    if (tries >= 5) return { error: 'Too many wrong codes. Close this page and press Connect again in Claude.' };
    const minute = Math.floor(now / 60000);
    const failKey = 'fails:' + minute;
    if (((await this.ctx.storage.get(failKey)) || 0) > 60) return { error: 'Too many attempts right now. Try again in a minute.' };

    const entry = await this.ctx.storage.get('code:' + code);
    if (!entry || entry.expires < now) {
      await this.ctx.storage.put('tries:' + attemptKey, tries + 1);
      await this.ctx.storage.put(failKey, ((await this.ctx.storage.get(failKey)) || 0) + 1);
      await this.ctx.storage.setAlarm(now + 120000);
      return { error: 'That code is not valid. Check the code on the Miyoo screen (it changes every 10 minutes).' };
    }
    return { deviceId: entry.deviceId };
  }

  async consume(code) {
    await this.ctx.storage.delete('code:' + code);
  }

  async alarm() {
    const now = Date.now();
    const minute = Math.floor(now / 60000);
    const fails = await this.ctx.storage.list({ prefix: 'fails:' });
    const old = [...fails.keys()].filter((k) => Number(k.slice(6)) < minute - 1);
    if (old.length) await this.ctx.storage.delete(old);
  }
}

/* ----------------------------------------------------------- pages */

const STYLE = `body{font-family:system-ui,sans-serif;max-width:28rem;margin:3rem auto;padding:0 1rem;color:#222}
h1{font-size:1.4rem}input[name=code]{font-size:2rem;letter-spacing:.4rem;width:10rem;text-align:center;padding:.3rem}
button{font-size:1.1rem;padding:.5rem 1.4rem;margin-top:1rem}.err{color:#b00020}.small{color:#666;font-size:.9rem}`;

function pairPage(handle, clientName, error) {
  return `<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connect your Miyoo</title><style>${STYLE}</style></head><body>
<h1>Connect your Miyoo to ${esc(clientName)}</h1>
<p>Open the <b>Claude Agent</b> app on your Miyoo and type the 6-digit code it shows.</p>
${error ? `<p class="err">${esc(error)}</p>` : ''}
<form method="post" action="/authorize">
<input type="hidden" name="handle" value="${esc(handle)}">
<input name="code" inputmode="numeric" pattern="[0-9 ]{6,7}" maxlength="7" autocomplete="one-time-code" autofocus required>
<br><button type="submit" name="decision" value="approve">Connect</button>
<button type="submit" name="decision" value="deny" formnovalidate>Cancel</button>
</form>
<p class="small">Connecting lets ${esc(clientName)} see the screen, press buttons, run commands and change files on this Miyoo.
You can disconnect every client at any time from the app on the Miyoo.</p>
</body></html>`;
}

function errorPage(text) {
  return `<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Claude Agent</title><style>${STYLE}</style></head><body><h1>Claude Agent</h1><p class="err">${esc(text)}</p></body></html>`;
}

const START_STYLE = `*{box-sizing:border-box}body{font-family:system-ui,-apple-system,sans-serif;max-width:34rem;margin:0 auto;padding:1.2rem 1rem 3rem;
color:#2b2b2b;background:#f6f4f1;line-height:1.45}h1{font-size:1.45rem;margin:.4rem 0 .2rem}h2{font-size:1.05rem;margin:1.6rem 0 .5rem}
.sub{color:#666;margin:0 0 1rem}.card{background:#fff;border-radius:14px;padding:1rem 1.1rem;box-shadow:0 1px 3px rgba(0,0,0,.08);margin:.8rem 0}
.addr #u{font:600 1rem ui-monospace,monospace;padding:.7rem .7rem;border:1px solid #ddd;border-radius:10px;background:#faf9f7;color:#c15f3c;
word-break:break-all;user-select:all;-webkit-user-select:all}.addr button{display:block;width:100%;margin-top:.55rem;font:600 1.05rem system-ui,sans-serif;
padding:.7rem 1rem;border:0;border-radius:10px;background:#c15f3c;color:#fff;cursor:pointer}.addr button.ok{background:#3f8f5a}
ol{padding-left:1.3rem;margin:.3rem 0}li{margin:.45rem 0}b{color:#111}.note{font-size:.9rem;color:#666}
.tip{background:#fff7e8;border-left:4px solid #e3a33c;padding:.6rem .8rem;border-radius:8px;font-size:.92rem}
.tabs{display:flex;gap:.4rem;margin-top:1rem}.tabs a{flex:1;text-align:center;padding:.5rem;border-radius:10px;background:#e9e5df;color:#333;
text-decoration:none;font-weight:600}`;

function homePage(origin, nonce) {
  const mcp = `${origin}/mcp`;
  return `<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connect your Miyoo to Claude</title><style>${START_STYLE}</style></head><body>
<h1>Connect your Miyoo to Claude</h1>
<p class="sub">This lets Claude see and help with your Miyoo. You decide what it can do: the Miyoo asks you before Claude changes anything.</p>

<div class="card"><b>1. Copy this address</b>
<p class="note" style="margin:.3rem 0 .6rem">It is the "connector" that links Claude to the Claude Agent app.</p>
<div class="addr"><div id="u">${esc(mcp)}</div><button id="c" type="button">Copy address</button></div>
</div>

<div class="card"><b>2. Add it to Claude</b>
<ol>
<li>Open the <b>Claude</b> app (or <b>claude.ai</b> on a computer) and sign in. A free account works.</li>
<li>Go to <b>Settings</b> &rarr; <b>Connectors</b>.</li>
<li>Tap <b>+</b> or <b>Add</b>, then <b>Add custom connector</b> (it may be under <b>Custom</b> &rarr; <b>Web</b>).</li>
<li>Name: <b>Miyoo</b>. Address: paste the one you copied. Tap <b>Add</b>.</li>
</ol>
<p class="tip">No "Add custom connector" in your phone app? Use the website instead: open a private (incognito) tab and type <b>claude.ai/settings</b> in the address bar, so the phone does not jump to the app. You only need the website once: after that, the connector works in the app too.</p>
</div>

<div class="card"><b>3. Type the code from your Miyoo</b>
<ol>
<li>Next to the new <b>Miyoo</b> connector, tap <b>Connect</b>.</li>
<li>A page asks for a code. Type the <b>6 numbers</b> shown on your Miyoo screen and tap <b>Connect</b>.</li>
<li>Done. The Miyoo shows <b>Connected</b>. You only do this once.</li>
</ol>
<p class="note">The code changes every 10 minutes. If it stops working, use the new one on the Miyoo.</p>
</div>

<div class="card"><b>4. Ask for help</b>
<p style="margin:.4rem 0 0">Open the <b>Claude Agent</b> app on the Miyoo and leave it open. Then start a chat in Claude (phone or computer), make sure the <b>Miyoo</b> connector is turned on in the chat's tools menu, and ask anything, like <i>"my game won't start, can you check?"</i></p>
<p class="note" style="margin:.6rem 0 0">Claude asks you before each step it takes on the Miyoo. If you prefer, open the <b>Miyoo</b> connector in Claude's <b>Settings</b> &rarr; <b>Connectors</b> and choose <b>Always allow</b>. The Miyoo will still ask you before Claude changes anything, as set in the Claude Agent app.</p>
</div>

<p class="note">Free Claude accounts can add one custom connector. Claude is an AI and can make mistakes: keep a backup copy of your SD card.</p>
<script nonce="${nonce}">
(function(){var b=document.getElementById('c'),u=document.getElementById('u'),t=u.textContent;
function pick(){var r=document.createRange();r.selectNodeContents(u);var g=window.getSelection();g.removeAllRanges();g.addRange(r)}
function done(){b.textContent='Copied!';b.className='ok';setTimeout(function(){b.textContent='Copy address';b.className=''},2500)}
function legacy(){pick();try{if(document.execCommand('copy'))done()}catch(e){}}
b.addEventListener('click',function(){if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(t).then(done,legacy)}else legacy()});})();
</script>
</body></html>`;
}

/* ------------------------------------------------- default handler (pages + device API) */

const defaultHandler = {
  async fetch(request, env) {
    const url = new URL(request.url);
    const oauth = env.OAUTH_PROVIDER;

    if ((url.pathname === '/' || url.pathname === '/start') && request.method === 'GET') {
      const nonce = randomHex(16);
      const h = new Headers();
      h.set('Content-Security-Policy', `default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}'`);
      return html(homePage(url.origin, nonce), 200, h);
    }

    if (url.pathname === '/api/device/register' && request.method === 'POST') {
      const deviceId = randomHex(16);
      const secret = randomHex(32);
      const ok = await deviceStub(env, deviceId).init(await sha256Hex(secret), deviceId);
      if (!ok) return json({ error: 'try again' }, 500);
      return json({ device_id: deviceId, device_secret: secret });
    }

    if (url.pathname === '/api/device/ws') {
      if (request.headers.get('Upgrade') !== 'websocket') return new Response('expected websocket', { status: 426 });
      const deviceId = request.headers.get('X-Device-Id') || '';
      if (!/^[0-9a-f]{32}$/.test(deviceId)) return new Response('bad device id', { status: 400 });
      return deviceStub(env, deviceId).fetch(request);
    }

    if (url.pathname === '/authorize') {
      try {
        if (request.method === 'GET') {
          const authReq = await oauth.parseAuthRequest(request);
          const details = await oauth.describeConsent(authReq);
          const consent = await oauth.beginConsent(authReq);
          return html(pairPage(consent.handle, details.clientName || 'Claude'), 200, consent.headers);
        }
        if (request.method === 'POST') {
          const form = await request.formData();
          const handle = String(form.get('handle') || '');
          if (form.get('decision') !== 'approve') {
            const denied = await oauth.denyConsent(request, handle);
            return new Response(null, { status: 302, headers: denied.headers });
          }
          const code = String(form.get('code') || '').replace(/\D/g, '');
          const r = await pairingStub(env).redeem(code, 'h:' + (await sha256Hex(handle)));
          if (r.error) return html(pairPage(handle, 'Claude', r.error), 400);
          const approved = await oauth.approveConsent(request, handle, { scope: [SCOPE] });
          const room = deviceStub(env, r.deviceId);
          const gen = await room.generation();
          const client = await oauth.lookupClient(approved.request.clientId).catch(() => null);
          const { redirectTo } = await oauth.completeAuthorization({
            request: approved.request,
            userId: r.deviceId,
            metadata: { paired: Date.now() },
            scope: [SCOPE],
            props: { deviceId: r.deviceId, gen },
            revokeExistingGrants: false,
          });
          await pairingStub(env).consume(code);
          await room.notifyPaired(client?.clientName);
          approved.headers.set('Location', redirectTo);
          return new Response(null, { status: 302, headers: approved.headers });
        }
      } catch (e) {
        if (e && e.redirectTo) return Response.redirect(e.redirectTo, 302);
        const why = e && e.message ? e.message + '. ' : '';
        return html(errorPage(why + 'Close this page and press Connect again in Claude (Settings > Connectors).'), 400);
      }
    }

    return new Response('Not found', { status: 404 });
  },
};

/* ------------------------------------------------------------- MCP endpoint */

function rpcResult(id, result) {
  return { jsonrpc: '2.0', id, result };
}
function rpcError(id, code, message) {
  return { jsonrpc: '2.0', id, error: { code, message } };
}

const offlineText =
  'The Miyoo is not connected right now. Ask the user to turn on Wi-Fi on the Miyoo and open the Claude Agent app (Apps menu), then try again.';
const revokedText = 'This connection was turned off on the Miyoo. The user has to connect again (pair with a new code).';

async function handleRpc(msg, env, props) {
  const { id, method, params } = msg;
  const room = deviceStub(env, props.deviceId);
  switch (method) {
    case 'initialize':
      return rpcResult(id, {
        protocolVersion: params?.protocolVersion || '2025-06-18',
        capabilities: { tools: { listChanged: false } },
        serverInfo: { name: 'claude-agent-miyoo', title: 'Miyoo (Claude Agent)', version: VERSION },
        instructions: SERVER_INSTRUCTIONS,
      });
    case 'ping':
      return rpcResult(id, {});
    case 'tools/list': {
      const t = await room.tools(props.gen);
      if (t.revoked) return rpcError(id, -32001, revokedText);
      return rpcResult(id, { tools: t.tools });
    }
    case 'tools/call': {
      const name = params?.name;
      const args = params?.arguments || {};
      // every call may first wait for the user to answer a permission question on the device (up to 90 s)
      let wait = 60000;
      if (name === 'run_command') wait = (Math.min(Number(args.timeout_s) || 30, 600) + 15) * 1000;
      if (name === 'press_buttons') wait = 120000;
      if (['download_file', 'input_play', 'run_app', 'perf_monitor', 'screen_capture', 'watch_file'].includes(name)) wait = 110000;
      wait += 95000;
      const r = await room.call(props.gen, name, args, Math.min(wait, MAX_TOOL_WAIT_MS));
      if (r.revoked) return rpcResult(id, { content: [{ type: 'text', text: revokedText }], isError: true });
      if (r.offline) return rpcResult(id, { content: [{ type: 'text', text: offlineText }], isError: true });
      return rpcResult(id, r.result);
    }
    default:
      if (id === undefined) return null; // notification
      return rpcError(id, -32601, `Method not found: ${method}`);
  }
}

const mcpHandler = {
  async fetch(request, env, ctx) {
    if (request.method !== 'POST') {
      return new Response('This MCP server only answers POST requests.', { status: 405, headers: { Allow: 'POST' } });
    }
    const props = ctx.props;
    if (!ctx.auth?.scope?.includes(SCOPE) || !props?.deviceId) return new Response('forbidden', { status: 403 });
    let body;
    try {
      body = await request.json();
    } catch {
      return json(rpcError(null, -32700, 'Parse error'), 400);
    }
    const batch = Array.isArray(body);
    const msgs = batch ? body : [body];
    const out = [];
    for (const m of msgs) {
      const r = await handleRpc(m, env, props);
      if (r) out.push(r);
    }
    if (!out.length) return new Response(null, { status: 202 });
    return json(batch ? out : out[0]);
  },
};

/* ------------------------------------------------------------- the Worker */

let provider = null;
let providerOrigin = null;

// One relay = one public address. The OAuth library keeps its helpers on the
// shared env object, so a second address in the same isolate would get the
// first one's settings: set PUBLIC_ORIGIN (the deploy script does) or the
// first address seen is the only one answered.
function providerFor(origin) {
  if (!provider) {
    providerOrigin = origin;
    provider = new OAuthProvider({
      apiRoute: '/mcp',
      apiHandler: mcpHandler,
      defaultHandler,
      authorizeEndpoint: '/authorize',
      tokenEndpoint: '/oauth/token',
      clientRegistrationEndpoint: '/oauth/register',
      clientIdMetadataDocumentEnabled: true,
      scopesSupported: [SCOPE],
      requiredScopes: [SCOPE],
      accessTokenTTL: 24 * 3600,
      // connect once: the sign-in lasts while it is used, and ends after 180 days without use
      refreshTokenTTL: 180 * 24 * 3600,
      refreshTokenIdleTTL: 180 * 24 * 3600,
      resourceMetadata: { resource: origin + '/mcp', authorization_servers: [origin] },
    });
  }
  return provider;
}

export default {
  fetch(request, env, ctx) {
    const origin = (env.PUBLIC_ORIGIN || new URL(request.url).origin).replace(/\/+$/, '');
    const p = providerFor(origin);
    if (origin !== providerOrigin) return new Response('Use ' + providerOrigin, { status: 421 });
    return p.fetch(request, env, ctx);
  },
};
