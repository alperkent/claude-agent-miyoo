#!/bin/bash
# Publishes your own Claude Agent relay on your Cloudflare account (the free plan is enough).
#
#   bash server/deploy.sh
#
# The first time it opens the browser to sign in to Cloudflare, creates the
# storage the relay needs (one KV namespace) and writes its id to
# wrangler.jsonc. At the end it prints the relay address and writes it to
# ../server.txt, so the next build of the app uses it by default.
#
# Change the "name" in wrangler.jsonc first if you want a different address
# (https://<name>.<your-subdomain>.workers.dev).
set -euo pipefail
cd "$(dirname "$0")"
[ -s "$HOME/.nvm/nvm.sh" ] && . "$HOME/.nvm/nvm.sh"
command -v node > /dev/null || { echo "ERROR: Node.js is not installed (https://nodejs.org, version 20 or newer)"; exit 1; }

echo "== installing the tools (slow only the first time)"
npm ci --no-fund --no-audit --loglevel=error

if ! npx wrangler whoami 2>&1 | grep -qi "logged in"; then
    echo "== signing in to Cloudflare (a browser window opens)"
    npx wrangler login
fi

if grep -q REPLACE_WITH_YOUR_KV_ID wrangler.jsonc; then
    echo "== creating the OAUTH_KV storage"
    OUT=$(npx wrangler kv namespace create OAUTH_KV 2>&1) || { echo "$OUT"; exit 1; }
    ID=$(echo "$OUT" | grep -oE '[0-9a-f]{32}' | head -n 1)
    [ -n "$ID" ] || { echo "$OUT"; echo "ERROR: could not read the new KV id"; exit 1; }
    sed -i.bak "s/REPLACE_WITH_YOUR_KV_ID/$ID/" wrangler.jsonc && rm -f wrangler.jsonc.bak
    echo "   KV id $ID written to wrangler.jsonc"
fi

echo "== publishing"
OUT=$(mktemp)
WRANGLER_OUTPUT_FILE_PATH=$OUT npx wrangler deploy
URL=$(grep -o 'https://[A-Za-z0-9.-]*workers\.dev' "$OUT" | head -n 1 || true)
rm -f "$OUT"
[ -n "$URL" ] || { echo "ERROR: could not find the relay address in wrangler's output"; exit 1; }

echo "== saving the public address ($URL)"
npx wrangler deploy --var "PUBLIC_ORIGIN:$URL" > /dev/null
echo "$URL" > ../server.txt

echo
echo "DONE."
echo "Relay address (for the Miyoo):  $URL"
echo "Connector address (for Claude): $URL/mcp"
