// End-to-end test of the relay: run `npm test` in server/ (starts wrangler dev on 127.0.0.1:8787).
import { createRequire } from 'module';
import crypto from 'crypto';
const require = createRequire(new URL('../package.json', import.meta.url));
const WebSocket = require('ws');
const B = 'http://127.0.0.1:8787';
const ok = (c, m) => { if (!c) { console.error('FAIL', m); process.exit(1); } console.log('ok  ', m); };

// 1. device registers and connects
let r = await fetch(B + '/api/device/register', { method: 'POST' });
const dev = await r.json();
ok(r.status === 200 && dev.device_id, 'device registers');
const ws = new WebSocket(B.replace('http', 'ws') + '/api/device/ws', {
  headers: { Authorization: 'Bearer ' + dev.device_secret, 'X-Device-Id': dev.device_id },
});
const inbox = [];
let waiter = null;
ws.on('message', (d) => {
  const m = JSON.parse(d.toString());
  if (m.type === 'call') ws.send(JSON.stringify({ type: 'result', id: m.id, result: { content: [{ type: 'text', text: 'pong:' + m.tool }] } }));
  inbox.push(m);
  if (waiter) waiter();
});
await new Promise((res, rej) => { ws.on('open', res); ws.on('error', rej); });
const next = (type) => new Promise((res) => {
  const look = () => { const i = inbox.findIndex((m) => m.type === type); if (i >= 0) { waiter = null; res(inbox.splice(i, 1)[0]); } };
  waiter = look; look();
});
ws.send(JSON.stringify({ type: 'hello', tools: [{ name: 'device_info', description: 'x', inputSchema: { type: 'object' } }], info: {} }));
ok((await next('welcome')).server.endsWith('-self'), 'device connected, welcome received');
ws.send(JSON.stringify({ type: 'pair_request' }));
const code = (await next('pair_code')).code;
ok(/^\d{6}$/.test(code), 'pairing code ' + code);

// wrong secret is refused
const bad = new WebSocket(B.replace('http', 'ws') + '/api/device/ws', { headers: { Authorization: 'Bearer 00', 'X-Device-Id': dev.device_id } });
ok(await new Promise((res) => { bad.on('open', () => res(false)); bad.on('error', () => res(true)); bad.on('unexpected-response', () => res(true)); }), 'wrong device secret refused');

// 2. OAuth client registration and authorization
async function client(redirect) {
  const r = await fetch(B + '/oauth/register', { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ redirect_uris: [redirect], client_name: 'Claude', token_endpoint_auth_method: 'none' }) });
  return r.json();
}
const pkce = () => { const v = crypto.randomBytes(32).toString('base64url'); return { v, c: crypto.createHash('sha256').update(v).digest('base64url') }; };
const authUrl = (cid, redirect, p) => `${B}/authorize?response_type=code&client_id=${encodeURIComponent(cid)}&redirect_uri=${encodeURIComponent(redirect)}&code_challenge=${p.c}&code_challenge_method=S256&scope=miyoo&state=xyz&resource=${encodeURIComponent(B + '/mcp')}`;

const evil = await client('https://evil.example/cb');
r = await fetch(authUrl(evil.client_id, 'https://evil.example/cb', pkce()));
ok(r.status === 403 && (await r.text()).includes('only accepts connections from Claude'), 'non-Claude client refused (403)');

const RED = 'https://claude.ai/api/mcp/auth_callback';
const c = await client(RED);
ok(c.client_id, 'Claude client registered');
const p = pkce();
r = await fetch(authUrl(c.client_id, RED, p));
const page = await r.text();
ok(r.status === 200 && page.includes('6-digit code'), 'pairing page shown');
const handle = page.match(/name="handle" value="([^"]+)"/)[1];
const cookie = (r.headers.getSetCookie?.() || []).map((x) => x.split(';')[0]).join('; ');
// wrong code first
const post = (code) => fetch(B + '/authorize', { method: 'POST', redirect: 'manual', headers: { Cookie: cookie, 'Content-Type': 'application/x-www-form-urlencoded', Origin: B },
  body: new URLSearchParams({ handle, code, decision: 'approve' }) });
r = await post('000000');
ok(r.status === 400, 'wrong code refused');
r = await post(code);
const loc = r.headers.get('location') || '';
ok(r.status === 302 && loc.startsWith(RED), 'right code redirects to Claude: ' + loc.slice(0, 60));
ok((await next('paired')).client === 'Claude', 'Miyoo told it is paired');
const authCode = new URL(loc).searchParams.get('code');
r = await fetch(B + '/oauth/token', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
  body: new URLSearchParams({ grant_type: 'authorization_code', code: authCode, redirect_uri: RED, client_id: c.client_id, code_verifier: p.v, resource: B + '/mcp' }) });
const tok = await r.json();
ok(tok.access_token, 'access token issued');

// 3. MCP
const mcp = (body, t = tok.access_token) => fetch(B + '/mcp', { method: 'POST', headers: { Authorization: 'Bearer ' + t, 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
r = await mcp({ jsonrpc: '2.0', id: 1, method: 'tools/list' });
ok((await r.json()).result.tools[0].name === 'device_info', 'tools/list returns device tools');
r = await mcp({ jsonrpc: '2.0', id: 2, method: 'tools/call', params: { name: 'device_info', arguments: {} } });
ok((await r.json()).result.content[0].text === 'pong:device_info', 'tools/call reaches the device and back');
r = await mcp({ jsonrpc: '2.0', id: 3, method: 'tools/list' }, 'nope');
ok(r.status === 401, 'MCP without a valid token refused');

// 4. revoke from the device
ws.send(JSON.stringify({ type: 'unpair_all' }));
await next('unpaired');
r = await mcp({ jsonrpc: '2.0', id: 4, method: 'tools/list' });
ok((await r.json()).error?.code === -32001, 'Disconnect every Claude revokes the token');

// 5. device cap (MAX_DEVICES=5): one used above
let last;
for (let i = 0; i < 5; i++) last = await fetch(B + '/api/device/register', { method: 'POST' });
ok(last.status === 403, 'device cap enforced');
ws.close();
console.log('ALL PASSED');
process.exit(0);
