#!/bin/sh
# Turns Wi-Fi on for the Claude Agent when it is off, and off again on exit.
# Same steps as RedOnion's updater (ota_update.sh) and update_networking.sh.
#   wifi.sh on       exit 0 = this script turned Wi-Fi on (and it has an address)
#   wifi.sh restore  turns it off again if it is off in Settings
sysdir=/mnt/SDCARD/.tmp_update

ip_now() { ip route get 1 2> /dev/null | awk '{print $NF;exit}'; }
wifi_setting_on() { [ "$(/customer/app/jsonval wifi 2> /dev/null)" -eq 1 ] 2> /dev/null; }
radio_running() { [ -e /sys/class/net/wlan0 ] && pgrep -f "wpa_supplicant.*wlan0" > /dev/null 2>&1; }
wait_for() { # seconds test
    i=0
    while [ $i -lt $(($1 * 2)) ]; do
        $2 && return 0
        sleep 0.5
        i=$((i + 1))
    done
    return 1
}
has_wlan0() { [ -e /sys/class/net/wlan0 ]; }
linked() { [ "$(cat /sys/class/net/wlan0/operstate 2> /dev/null)" = "up" ]; }
has_ip() { [ -n "$(ip_now)" ]; }

case "$1" in
on)
    has_ip && exit 1
    if radio_running; then
        wait_for 20 has_ip
        exit 1
    fi
    insmod /mnt/SDCARD/8188fu.ko 2> /dev/null
    ifconfig lo up
    /customer/app/axp_test wifion > /dev/null 2>&1
    wait_for 10 has_wlan0
    sleep 2
    ifconfig wlan0 up
    [ -x $sysdir/script/network/wifi_power_save.sh ] && $sysdir/script/network/wifi_power_save.sh auto
    pkill -9 wpa_supplicant 2> /dev/null
    /mnt/SDCARD/miyoo/app/wpa_supplicant -B -D nl80211 -iwlan0 -c /appconfigs/wpa_supplicant.conf
    wait_for 50 linked
    pkill -9 udhcpc 2> /dev/null
    udhcpc -i wlan0 -s /etc/init.d/udhcpc.script > /dev/null 2>&1 &
    wait_for 20 has_ip
    exit 0
    ;;
restore)
    wifi_setting_on && exit 0
    pkill -9 wpa_supplicant 2> /dev/null
    pkill -9 udhcpc 2> /dev/null
    /customer/app/axp_test wifioff > /dev/null 2>&1
    ;;
esac
