#!/bin/sh
# Build the Claude Agent for the Miyoo Mini family (OnionOS / RedOnion).
# Run inside the miyoomini-toolchain container:  sh build.sh
# The first time it downloads, at fixed versions and checked:
#   cJSON (MIT), stb_image_write (public domain), QR Code generator (MIT), Mbed TLS 3.6.7 (Apache-2.0)
#   and the Mozilla list of certificate authorities (from certifi, MPL-2.0).
# If the file server.txt exists next to this script, its address becomes the
# relay the agent uses by default. UPDATE_REPO (environment, or update_repo.txt)
# sets the GitHub repository the app takes updates from.
# Result: package/App/ClaudeAgent/ (the whole app folder for the SD card)
set -e
cd "$(dirname "$0")"
# docker exec without a login shell does not load the toolchain PATH
export PATH="/opt/miyoomini-toolchain/usr/bin:$PATH"
CC=${CC:-arm-linux-gnueabihf-gcc}
AR=${AR:-arm-linux-gnueabihf-ar}
STRIP=${STRIP:-arm-linux-gnueabihf-strip}
command -v $CC > /dev/null || { echo "ERRO: compilador $CC nao encontrado"; exit 1; }
CFLAGS="-Os -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -march=armv7ve+simd"

CJSON=https://raw.githubusercontent.com/DaveGamble/cJSON/6d9f2443ab071f86e5d9b43025a40929ec41c46c
STB=https://raw.githubusercontent.com/nothings/stb/78770d85c4d94787a56c7859017da963bc4f3439
MBED=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7
QRGEN=https://raw.githubusercontent.com/nayuki/QR-Code-generator/720f62bddb7226106071d4728c292cb1df519ceb/c
CERTS=https://raw.githubusercontent.com/certifi/python-certifi/2026.10.23/certifi

mkdir -p third_party
get() { # url file md5
    if [ ! -f "third_party/$2" ] || ! echo "$3  third_party/$2" | md5sum -c - > /dev/null 2>&1; then
        echo "baixando $2"
        wget -q -O "third_party/$2" "$1/$2"
        echo "$3  third_party/$2" | md5sum -c - > /dev/null || { echo "ERRO: $2 veio diferente"; exit 1; }
    fi
}
get $CJSON cJSON.c d3aac57ffab939e1fe9e2c904f175ee6
get $CJSON cJSON.h 8a34ff93cf64864c57e119edf7700b91
get $STB stb_image_write.h db165e05e694c0e9f2b46b94fecf4e10
get $STB stb_truetype.h 7d13bfa54c8e64c2b66974d6a359b28b
get $QRGEN qrcodegen.c 0988871d46b1df073d8acc8bdc6f63fd
get $QRGEN qrcodegen.h 669e5d1fdde4c7ce77906a72f517d619
get $MBED mbedtls-3.6.7.tar.bz2 d207524bd8f9dfb5fbf7f4a1997e636f
get $CERTS cacert.pem dcad88ed195858fae5fe15f24961766e

# Mbed TLS, compiled once
M=third_party/mbedtls-3.6.7
# threading on: downloads run next to the connection, in another thread
MBFLAGS="-DMBEDTLS_THREADING_C -DMBEDTLS_THREADING_PTHREAD"
LIB=third_party/libmbedtls-arm-mt.a
if [ ! -f $LIB ]; then
    echo "compilando Mbed TLS (so na primeira vez, demora um pouco)"
    rm -rf $M third_party/mbedtls-obj
    tar xjf third_party/mbedtls-3.6.7.tar.bz2 -C third_party
    mkdir -p third_party/mbedtls-obj
    for f in $M/library/*.c; do
        $CC $CFLAGS $MBFLAGS -I$M/include -I$M/library -c "$f" -o third_party/mbedtls-obj/$(basename "$f" .c).o
    done
    $AR rcs $LIB third_party/mbedtls-obj/*.o
fi

DEF=""
if [ -f server.txt ]; then
    SERVER=$(head -n 1 server.txt | tr -d '\r\n ')
    DEF="-DDEFAULT_SERVER=\"$SERVER\""
    echo "relay: $SERVER"
fi
# Repository the app checks for updates (owner/name). Without it the app
# follows the original author's releases.
UPDATE_REPO=${UPDATE_REPO:-}
if [ -z "$UPDATE_REPO" ] && [ -f update_repo.txt ]; then
    UPDATE_REPO=$(head -n 1 update_repo.txt | tr -d '\r\n ')
fi
if [ -n "$UPDATE_REPO" ]; then
    DEF="$DEF -DUPDATE_REPO=\"$UPDATE_REPO\""
    echo "updates from: $UPDATE_REPO"
fi

OUT=package/App/ClaudeAgent
mkdir -p $OUT
$CC $CFLAGS $MBFLAGS -Wall -Wno-unused-function -Wno-format-truncation -Ithird_party -I$M/include $DEF \
    -o $OUT/claude-agent src/agent.c src/net.c src/ui.c src/perm.c src/dev.c src/changes.c src/update.c src/devrun.c src/plugins.c src/i18n.c src/backups.c third_party/cJSON.c third_party/qrcodegen.c $LIB -lm -lpthread
$STRIP $OUT/claude-agent
cp third_party/cacert.pem $OUT/cacert.pem
cp app/config.json app/launch.sh app/wifi.sh $OUT/
mkdir -p $OUT/lang
cp app/lang/*.txt $OUT/lang/
base64 -d app/icon.png.b64 > $OUT/icon.png
chmod +x $OUT/launch.sh $OUT/wifi.sh $OUT/claude-agent
echo "OK"
md5sum src/*.c src/common.h $OUT/claude-agent
