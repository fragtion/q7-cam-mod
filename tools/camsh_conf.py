# Camera address for the tools in this folder: set Q7_HOST (and optionally Q7_PORT) in the
# environment, or edit the defaults here. Port 23 = busybox telnetd started by boot.sh;
# 9999 = the raw inetd shell boot.sh also starts as a fallback.
import os

HOST = os.environ.get("Q7_HOST", "192.168.1.100")
PORT = int(os.environ.get("Q7_PORT", "23"))
