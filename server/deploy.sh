#!/bin/bash
# Publishes the Claude Agent relay on your Cloudflare account (free plan).
# Run in WSL:  bash ~/fallout-miyoo/claude-agent/server/deploy.sh
# The first time it opens the browser to sign in to Cloudflare.
# At the end it writes the relay address to ../server.txt, so the next
# build of the agent uses it by default.
set -e
cd "$(dirname "$0")"
[ -s "$HOME/.nvm/nvm.sh" ] && . "$HOME/.nvm/nvm.sh"
command -v node > /dev/null || { echo "ERRO: o Node.js nao esta instalado"; exit 1; }
md5sum src/index.js

echo "== instalando as ferramentas (so demora na primeira vez)"
npm install --no-fund --no-audit --loglevel=error

if ! npx wrangler whoami 2>&1 | grep -qi "logged in"; then
    echo "== entrando na sua conta Cloudflare (vai abrir o navegador)"
    npx wrangler login
fi

echo "== publicando"
OUT=$(mktemp)
WRANGLER_OUTPUT_FILE_PATH=$OUT npx wrangler deploy
URL=$(grep -o 'https://[A-Za-z0-9.-]*workers\.dev' "$OUT" | head -n 1)
rm -f "$OUT"
[ -n "$URL" ] || { echo "ERRO: nao achei o endereco do servidor na saida do wrangler"; exit 1; }

echo "== gravando o endereco publico ($URL)"
npx wrangler deploy --var PUBLIC_ORIGIN:$URL > /dev/null
echo "$URL" > ../server.txt

echo
echo "PRONTO."
echo "Endereco do conector: $URL/mcp"
