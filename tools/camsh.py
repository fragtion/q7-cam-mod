"""Run shell commands on the camera:  python tools/camsh.py "cmd; cmd" [timeout_s]

Interactive use: telnet <camera-ip> (port 23). Host/port live in camsh_conf.py.
"""
import sys

from camconn import Cam

cmd = sys.argv[1] if len(sys.argv) > 1 else "echo hi"
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
try:
    cam = Cam()
except OSError as e:
    print("CONNECT FAIL:", e)
    sys.exit(1)
sys.stdout.write(cam.run(cmd, timeout))
cam.close()
