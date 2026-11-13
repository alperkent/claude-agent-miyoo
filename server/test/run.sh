#!/bin/bash
# Starts the relay locally (wrangler dev) and runs the end-to-end test against it.
set -u
cd "$(dirname "$0")/.."
LOG=$(mktemp)
STATE=$(mktemp -d)
node_modules/.bin/wrangler dev --port 8787 --ip 127.0.0.1 --persist-to "$STATE" > "$LOG" 2>&1 &
PID=$!
stop() {
    pkill -P $PID 2>/dev/null; kill $PID 2>/dev/null
    pkill -f "persist-to $STATE" 2>/dev/null
    rm -rf "$LOG" "$STATE"
}
trap stop EXIT
for i in $(seq 1 60); do
    grep -q "Ready on" "$LOG" && break
    kill -0 $PID 2>/dev/null || { cat "$LOG"; exit 1; }
    sleep 1
done
grep -q "Ready on" "$LOG" || { cat "$LOG"; echo "wrangler dev did not start"; exit 1; }
node test/e2e.mjs
