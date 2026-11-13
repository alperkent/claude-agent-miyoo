#!/usr/bin/env node
// Publishes your own Claude Agent relay on your Cloudflare account (the free plan is enough).
// Works the same on Windows (PowerShell or cmd), macOS and Linux:
//
//   node server/deploy.mjs
//
// The first time it opens the browser to sign in to Cloudflare, creates the
// storage the relay needs (one KV namespace) and writes its id to
// wrangler.jsonc. At the end it prints the relay address and writes it to
// ../server.txt, so the next build of the app uses it by default.
//
// Change the "name" in wrangler.jsonc first if you want a different address
// (https://<name>.<your-subdomain>.workers.dev).
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
process.chdir(here);

function fail(msg, output) {
  if (output) console.error(output);
  console.error('ERROR: ' + msg);
  process.exit(1);
}

// Runs a command; with capture, returns its output (stdout + stderr) instead of showing it.
function run(cmd, { capture = false, env } = {}) {
  const r = spawnSync(cmd, {
    shell: true, // npm and npx are .cmd files on Windows
    stdio: capture ? ['inherit', 'pipe', 'pipe'] : 'inherit',
    encoding: 'utf8',
    env: { ...process.env, ...env },
  });
  if (r.error) fail(`could not run "${cmd}": ${r.error.message}`);
  return { status: r.status, out: (r.stdout || '') + (r.stderr || '') };
}

const [major] = process.versions.node.split('.').map(Number);
if (major < 20) fail(`Node.js 20 or newer is needed (this is ${process.versions.node}): https://nodejs.org`);

console.log('== installing the tools (slow only the first time)');
if (run('npm ci --no-fund --no-audit --loglevel=error').status !== 0) fail('npm ci failed');

const who = run('npx wrangler whoami', { capture: true });
if (!/logged in/i.test(who.out) || /not logged in/i.test(who.out)) {
  console.log('== signing in to Cloudflare (a browser window opens)');
  if (run('npx wrangler login').status !== 0) fail('Cloudflare sign-in failed');
}

const CONFIG = join(here, 'wrangler.jsonc');
let config = readFileSync(CONFIG, 'utf8');
if (config.includes('REPLACE_WITH_YOUR_KV_ID')) {
  console.log('== creating the OAUTH_KV storage');
  const kv = run('npx wrangler kv namespace create OAUTH_KV', { capture: true });
  if (kv.status !== 0) fail('could not create the KV namespace', kv.out);
  const id = (kv.out.match(/\b[0-9a-f]{32}\b/) || [])[0];
  if (!id) fail('could not read the new KV id', kv.out);
  config = config.replace('REPLACE_WITH_YOUR_KV_ID', id);
  writeFileSync(CONFIG, config);
  console.log(`   KV id ${id} written to wrangler.jsonc`);
}

console.log('== publishing');
const tmp = mkdtempSync(join(tmpdir(), 'miyoo-relay-'));
const outFile = join(tmp, 'wrangler-output.ndjson');
if (run('npx wrangler deploy', { env: { WRANGLER_OUTPUT_FILE_PATH: outFile } }).status !== 0) fail('wrangler deploy failed');
let url = '';
try {
  url = (readFileSync(outFile, 'utf8').match(/https:\/\/[A-Za-z0-9.-]*workers\.dev/) || [''])[0];
} catch {}
rmSync(tmp, { recursive: true, force: true });
if (!url) fail("could not find the relay address in wrangler's output");

console.log(`== saving the public address (${url})`);
const pub = run(`npx wrangler deploy --var PUBLIC_ORIGIN:${url}`, { capture: true });
if (pub.status !== 0) fail('second deploy (PUBLIC_ORIGIN) failed', pub.out);
writeFileSync(join(here, '..', 'server.txt'), url + '\n');

console.log('\nDONE.');
console.log(`Relay address (for the Miyoo):  ${url}`);
console.log(`Connector address (for Claude): ${url}/mcp`);
