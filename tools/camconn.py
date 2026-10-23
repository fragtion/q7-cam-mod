"""Shell connection to the camera, shared by camsh.py and camput.py.

Speaks to busybox telnetd on port 23 (declining every telnet option), or to the
old raw inetd shell on 9999 if PORT is set to that. run() sends a command and
returns its output up to an end marker, with the echoed command line removed.
"""
import socket
import time

from camsh_conf import HOST, PORT

IAC, WILL, WONT, DO, DONT, SB, SE = 255, 251, 252, 253, 254, 250, 240
MARK = "ZZEND"


class Cam:
    def __init__(self, host=HOST, port=PORT, timeout=8.0):
        self.s = socket.create_connection((host, port), timeout=6)
        self.s.settimeout(0.5)
        self.telnet = port != 9999
        self.pending = b""
        self.read_until(None, 1.0)          # banner / prompt / negotiation

    def _filter(self, data):
        """Strip telnet commands from data, refusing any option the server offers."""
        if not self.telnet:
            return data
        data = self.pending + data
        self.pending = b""
        out, reply, i = bytearray(), bytearray(), 0
        while i < len(data):
            b = data[i]
            if b != IAC:
                out.append(b); i += 1; continue
            if i + 1 >= len(data):
                self.pending = data[i:]; break
            cmd = data[i + 1]
            if cmd in (WILL, WONT, DO, DONT):
                if i + 2 >= len(data):
                    self.pending = data[i:]; break
                opt = data[i + 2]
                if cmd == WILL: reply += bytes([IAC, DONT, opt])
                elif cmd == DO: reply += bytes([IAC, WONT, opt])
                i += 3
            elif cmd == SB:
                end = data.find(bytes([IAC, SE]), i)
                if end < 0:
                    self.pending = data[i:]; break
                i = end + 2
            elif cmd == IAC:
                out.append(IAC); i += 2
            else:
                i += 2
        if reply:
            self.s.sendall(bytes(reply))
        return bytes(out)

    def read_until(self, marker, timeout):
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                if marker is None and buf:
                    break
                continue
            if not d:
                break
            buf += self._filter(d)
            if marker and marker.encode() in buf:
                break
        return buf.decode("latin1")

    def send(self, text):
        self.s.sendall(text.replace("\n", "\r\n").encode() if self.telnet else text.encode())

    def run(self, cmd, timeout=8.0):
        self.send(f"{cmd}\necho {MARK}$((1+1))\n")   # $((1+1)) so the echoed command never matches
        out = self.read_until(MARK + "2", timeout)
        out = out.split(MARK + "2")[0]
        lines = out.replace("\r", "").split("\n")
        # drop echoed input lines (telnet pty echoes what we type)
        sent = set(l.strip() for l in cmd.split("\n")) | {f"echo {MARK}$((1+1))"}
        lines = [l for l in lines if l.strip() not in sent and not l.strip().endswith(f"echo {MARK}$((1+1))")]
        return "\n".join(lines).strip("\n") + "\n"

    def close(self):
        self.s.close()
