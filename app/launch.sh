#!/bin/sh
# Claude Agent: opened from the Apps menu.
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="/lib:/config/lib:/mnt/SDCARD/miyoo/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"
# no sleep timer while the app is open (OnionOS keymon checks this file)
touch /tmp/stay_awake
mkdir -p logs
./claude-agent ui > logs/ui.out 2>&1
rm -f /tmp/stay_awake
