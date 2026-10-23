#!/bin/sh
# Boot bootstrap, run by the firmware's rcS INJECT once the SD card is mounted at /mnt/mmc.
# The card root keeps only what has to be there: this file (the INJECT runs /mnt/mmc/boot.sh),
# wifi.txt (read by the vendor's update.exe) and RECORD/ (clips). Everything else is in q7mod/.
Q=/mnt/mmc/q7mod
[ -d $Q ] || Q=/mnt/mmc                  # old layout: everything at the root
echo $Q > /tmp/.q7mod
BB=$Q/busybox

# keep SD writes down: reading a file must not update its FAT access date
mount -o remount,noatime /mnt/mmc 2>/dev/null

# LinuxIP's reset-button handler does "rm -rf /mnt/mmc/wifi.txt" (a misread pin is enough),
# leaving the camera off the network. Put it back from the spare copy before update.exe reads it.
if [ ! -s /mnt/mmc/wifi.txt ] && [ -s $Q/wifi.txt.keep ]; then
  cp $Q/wifi.txt.keep /mnt/mmc/wifi.txt; $BB sync
elif [ -s /mnt/mmc/wifi.txt ] && [ ! -s $Q/wifi.txt.keep ]; then
  cp /mnt/mmc/wifi.txt $Q/wifi.txt.keep; $BB sync     # first boot: make the spare copy
fi

# a camd update uploaded while camd was running (its binary is busy) lands as camd.new
if [ -s $Q/camd.new ]; then
  mv $Q/camd.new $Q/camd; $BB sync
fi

# root shell: telnet on tcp/23 (busybox telnetd, real pty; no login, LAN only)
$BB telnetd -p 23 -l /bin/sh
# raw shell on tcp/9999, a fallback lifeline if telnetd ever fails
echo '9999 stream tcp nowait root /bin/sh sh -i' >/tmp/s
inetd /tmp/s &

# Our camera daemon instead of the vendor LinuxIP (no cloud/P2P, less RAM). update.exe still
# brings up Wi-Fi; it launches a do-nothing stub in LinuxIP's place and pg.sh starts camd.
# Delete q7mod/use_camd (and power-cycle) to go back to the vendor LinuxIP.
if [ -x $Q/camd ] && [ -f $Q/use_camd ]; then
  printf '#!/bin/sh\nexec sleep 2147483647\n' > /test/LinuxIP.exe
  chmod +x /test/LinuxIP.exe
  touch /tmp/.camd
fi

# vendor boot: Wi-Fi (reads wifi.txt) + launches LinuxIP (or the stub above)
cd /
/test/update.exe &

# camd + audiod once Wi-Fi has settled
(sleep 15; sh $Q/pg.sh) &
