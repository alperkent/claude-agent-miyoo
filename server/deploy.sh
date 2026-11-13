#!/bin/sh
# Same as `node server/deploy.mjs` (which also works on Windows PowerShell).
cd "$(dirname "$0")" || exit 1
[ -s "$HOME/.nvm/nvm.sh" ] && . "$HOME/.nvm/nvm.sh"
exec node deploy.mjs "$@"
