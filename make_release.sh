#!/bin/sh
# Builds Claude Agent and packs the release zip: dist/ClaudeAgent-v<version>.zip
# The zip holds App/ClaudeAgent/..., ready to be extracted at the root of the SD card,
# and is also what the app downloads when it updates itself.
# Run in WSL:  sh ~/fallout-miyoo/claude-agent/make_release.sh
set -e
cd "$(dirname "$0")"
V=$(grep -o 'AGENT_VERSION "[^"]*"' src/common.h | cut -d'"' -f2)
echo "== building Claude Agent $V"
docker start miyoomini-toolchain-debug > /dev/null
docker exec miyoomini-toolchain-debug sh /root/workspace/claude-agent/build.sh | tail -n 2

P=package/App/ClaudeAgent
S=dist/stage/App/ClaudeAgent
rm -rf dist/stage
mkdir -p $S/lang
for f in claude-agent cacert.pem config.json launch.sh wifi.sh icon.png; do
    cp "$P/$f" "$S/"
done
cp "$P"/lang/*.txt "$S/lang/"
Z="ClaudeAgent-v$V.zip"
rm -f "dist/$Z"
(cd dist/stage && python3 -m zipfile -c "../$Z" App)
rm -rf dist/stage
echo "== dist/$Z"
python3 -m zipfile -l "dist/$Z"
sha256sum "dist/$Z"
