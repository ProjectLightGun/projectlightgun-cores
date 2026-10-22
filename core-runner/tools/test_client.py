"""
Pretends to be the app: starts the core runner, talks to it over the connection exactly as the
app will (protocol.h / docs/core-runner-contract.md), and checks every message type works.

  python core-runner/tools/test_client.py --runner <core-runner.exe> --core <fceumm dll> --game <plg.nes>

What it does, in order:
  HELLO (token checked) -> START -> READY -> ~3 s of pictures and sound -> aim at NEON SIEGE and
  pull the trigger -> PAUSE (no pictures may arrive) -> RESUME -> STOP -> STOPPED, runner exits.
It writes the pictures before and after the shot as PNGs, and prints frame and sound rates.
Exit code 0 = every check passed.
"""
import argparse, os, secrets, socket, struct, subprocess, sys, time, zlib

MSG_START, MSG_INPUT, MSG_PAUSE, MSG_RESUME, MSG_STOP = 1, 2, 3, 4, 5
MSG_HELLO, MSG_READY, MSG_FAILED, MSG_PICTURE, MSG_SHAPE, MSG_SOUND, MSG_LOG, MSG_STOPPED = range(64, 72)
NAMES = {64: "HELLO", 65: "READY", 66: "FAILED", 67: "PICTURE", 68: "SHAPE", 69: "SOUND", 70: "LOG", 71: "STOPPED"}
BTN_TRIGGER = 0x01

problems = []
def check(ok, what):
    print(("  ok    " if ok else "  FAIL  ") + what)
    if not ok:
        problems.append(what)


class Conn:
    def __init__(self, sock):
        self.sock, self.buf = sock, b""

    def send(self, mtype, payload=b""):
        self.sock.sendall(struct.pack("<IB", 1 + len(payload), mtype) + payload)

    def input(self, x, y, on_screen=True, buttons=0, sx=0, sy=0):
        ax, ay = round(x * 65535), round(y * 65535)
        self.send(MSG_INPUT, struct.pack("<HHBBhhH", ax, ay, 1 if on_screen else 0, buttons, sx, sy, 0))

    def recv(self, timeout):
        """One message, or None if nothing arrived in time. ('closed', b'') if the runner hung up."""
        end = time.time() + timeout
        while True:
            if len(self.buf) >= 5:
                n = struct.unpack_from("<I", self.buf)[0]
                if len(self.buf) >= 4 + n:
                    mtype, payload = self.buf[4], self.buf[5:4 + n]
                    self.buf = self.buf[4 + n:]
                    return mtype, payload
            left = end - time.time()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(1 << 20)
            except socket.timeout:
                return None
            except OSError:
                return "closed", b""
            if not data:
                return "closed", b""
            self.buf += data


def text(payload):
    return dict(line.split("=", 1) for line in payload.decode().split("\n") if "=" in line)


def write_png(path, payload):
    frame, w, h, fmt = struct.unpack_from("<IHHB", payload)
    px = payload[27:]   # protocol 2: 27-byte PICTURE header (protocol.h)
    bottom_up = fmt & 0x80
    fmt &= 0x7F
    rows = []
    for oy in range(h):
        y = h - 1 - oy if bottom_up else oy
        row = bytearray([0])
        for x in range(w):
            if fmt == 1:
                v = px[(y * w + x) * 2] | (px[(y * w + x) * 2 + 1] << 8)
                r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
                row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            else:
                row += px[(y * w + x) * 4:(y * w + x) * 4 + 3]
        rows.append(bytes(row))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


def collect(conn, seconds, verbose_logs=True):
    """Gather messages for a while. Returns (pictures, sound_frames, last_picture, other)."""
    pictures, sound, last, other = 0, 0, None, []
    end = time.time() + seconds
    while time.time() < end:
        m = conn.recv(end - time.time())
        if m is None:
            break
        mtype, payload = m
        if mtype == "closed":
            other.append(m)
            break
        if mtype == MSG_PICTURE:
            pictures += 1
            last = payload
        elif mtype == MSG_SOUND:
            sound += len(payload) // 4
        elif mtype == MSG_LOG:
            if verbose_logs:
                print("        runner: " + payload.decode(errors="replace"))
        else:
            other.append(m)
    return pictures, sound, last, other


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runner", required=True)
    ap.add_argument("--core", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", default="runner-client-out")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    token = secrets.token_hex(8)
    proc = subprocess.Popen([a.runner, "--connect", f"127.0.0.1:{port}", "--token", token])
    listener.settimeout(10)
    sock, _ = listener.accept()
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn = Conn(sock)

    print("Connection")
    m = conn.recv(5)
    check(m is not None and m[0] == MSG_HELLO, "HELLO arrives first")
    hello = text(m[1]) if m else {}
    check(hello.get("token") == token, "HELLO carries our token")

    save = os.path.join(a.out, "saves", "nes", "plg.srm")
    start = "\n".join([
        f"core={a.core}", f"game={a.game}", f"system_dir={a.out}", f"save_dir={a.out}",
        f"cart_save={save}", "port0=pad", "port1=zapper",
        "option=fceumm_zapper_mode=clightgun", "option=fceumm_show_crosshair=enabled",
        "option=fceumm_zapper_tolerance=6", "option=fceumm_up_down_allowed=enabled",
        "offscreen_signal=on"])
    conn.send(MSG_START, start.encode())
    ready = None
    while True:
        m = conn.recv(10)
        if m is None or m[0] in (MSG_READY, MSG_FAILED, "closed"):
            break
    check(m is not None and m[0] == MSG_READY, "READY after START")
    if m and m[0] == MSG_READY:
        ready = text(m[1])
        print(f"        {ready}")
    fps = float(ready["fps"]) if ready else 60.0
    rate = float(ready["sample_rate"]) if ready else 48000.0

    print("Playing (title screen)")
    conn.input(0.5, 0.5, on_screen=False)
    pics, snd, last, _ = collect(conn, 3.0)
    check(abs(pics / 3.0 - fps) < fps * 0.1, f"pictures at the game's speed ({pics / 3.0:.1f}/s, want ~{fps:.1f})")
    check(abs(snd / 3.0 - rate) < rate * 0.1, f"sound at the game's rate ({snd / 3.0:.0f}/s, want ~{rate:.0f})")
    if last:
        write_png(os.path.join(a.out, "before_shot.png"), last)
    before = last

    print("Shoot NEON SIEGE")
    x, y = 193 / 256, 163 / 240
    conn.input(x, y)
    collect(conn, 0.2, False)
    conn.input(x, y, buttons=BTN_TRIGGER)
    collect(conn, 0.15, False)
    conn.input(x, y)
    pics, snd, last, _ = collect(conn, 2.0, False)
    if last:
        write_png(os.path.join(a.out, "after_shot.png"), last)
    check(last is not None and before is not None and last[9:] != before[9:], "the picture changed after the shot")

    print("Pause / resume")
    conn.send(MSG_PAUSE)
    collect(conn, 0.2, False)   # anything already on its way
    pics, snd, _, _ = collect(conn, 1.0, False)
    check(pics == 0 and snd == 0, f"nothing arrives while paused ({pics} pictures, {snd} sound frames)")
    conn.send(MSG_RESUME)
    pics, _, _, _ = collect(conn, 1.0, False)
    check(pics > fps * 0.8, f"pictures again after resume ({pics})")

    print("Stop")
    conn.send(MSG_STOP)
    stopped = False
    end = time.time() + 5
    while time.time() < end:
        m = conn.recv(end - time.time())
        if m is None or m[0] == "closed":
            break
        if m[0] == MSG_STOPPED:
            stopped = True
        elif m[0] == MSG_LOG:
            print("        runner: " + m[1].decode(errors="replace"))
    check(stopped, "STOPPED after STOP")
    try:
        code = proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
        code = None
    check(code == 0, f"runner exited by itself (exit code {code})")
    check(os.path.exists(save), "cartridge save written")

    print()
    print("ALL PASSED" if not problems else f"{len(problems)} PROBLEM(S): " + "; ".join(problems))
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
