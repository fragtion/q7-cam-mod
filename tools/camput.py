"""Copy a local file to the camera over the shell (no HTTP server needed).

    python tools/camput.py <local file> <remote path> [mode]

Sends the file as base64 in 900-byte pieces, one shell command each (busybox ash
line editing truncates longer input lines), decodes it with busybox, and
checks the md5 on the camera against the local one. Host/port: camsh_conf.py.
"""
import base64, hashlib, re, sys

from camconn import Cam

BUSYBOX = "/tmp/busybox"   # linked to q7mod/busybox by pg.sh
CHUNK = 900   # busybox ash line editing truncates input lines at ~1 KB


def main():
    src, dst = sys.argv[1], sys.argv[2]
    mode = sys.argv[3] if len(sys.argv) > 3 else "755"
    data = open(src, "rb").read()
    want = hashlib.md5(data).hexdigest()
    b64 = base64.b64encode(data).decode()
    tmp = dst + ".b64"

    cam = Cam()
    cam.run(f"rm -f {tmp}")
    for i in range(0, len(b64), CHUNK):
        cam.run(f"printf %s '{b64[i:i + CHUNK]}' >> {tmp}", 15)
    out = cam.run(f"{BUSYBOX} base64 -d {tmp} > {dst} && rm {tmp}; chmod {mode} {dst}; {BUSYBOX} sync; {BUSYBOX} md5sum {dst}", 30)
    cam.close()

    m = re.search(r"\b([0-9a-f]{32})\s+" + re.escape(dst), out)
    got = m.group(1) if m else ""
    print(f"{dst}: {len(data)} bytes, md5 {'OK' if got == want else 'MISMATCH ' + (got or out.strip()[-200:]) + ' != ' + want}")
    sys.exit(0 if got == want else 1)


if __name__ == "__main__":
    main()
