#!/bin/sh
# Start camd (video, IR, motion, recording, settings, HTTP :8080) and audiod (audio :8081).
# Run by boot.sh; q7mod's path comes from /tmp/.q7mod. /tmp is RAM, so only the small
# binaries are copied there; busybox (1.2 MB) is linked from the SD card as /tmp/busybox.
Q=$(cat /tmp/.q7mod 2>/dev/null); [ -n "$Q" ] || Q=/mnt/mmc/q7mod
ln -sf $Q/busybox /tmp/busybox

# camd, kept running by a small supervisor; the log is O_APPEND so camd can truncate it in
# place. Time: camd runs busybox ntpd itself with its ntp_servers setting.
if [ -f /tmp/.camd ]; then
  cp $Q/camd /tmp/camd && chmod +x /tmp/camd
  if ! ps | grep -q '[/]tmp/camd'; then
    /tmp/busybox setsid sh -c 'n=0; while [ $n -lt 5 ]; do /tmp/camd >>/tmp/camd.log 2>&1; n=$((n+1)); echo "camd exited ($n)" >>/tmp/camd.log; sleep 10; done' &
  fi
fi

# audio on 8081
if [ -f $Q/audiod ]; then
  cp $Q/audiod /tmp/audiod 2>/dev/null; chmod +x /tmp/audiod
  if ! ps | grep -q '[/]tmp/audiod'; then
    /tmp/busybox setsid /tmp/audiod >/tmp/audiod.log 2>&1 &
  fi
fi
