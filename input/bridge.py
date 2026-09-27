#!/usr/bin/env python3
"""Client for ps2client --input bridge: drive a PS2 program from scripts.

Start the program with `ps2client --input bridge execee host:<elf>`, then:

    python bridge.py press start                    # tap START
    python bridge.py press cross r1 --lx -1 --for 2  # hold for 2 s
    python bridge.py --file game.csv list           # a project's maneuvers
    python bridge.py --file game.csv run roll-left climb
    python bridge.py tour                           # every control once
    python bridge.py module list                    # ps2client's input modules
    python bridge.py module load keyboard           # hot-swap: load, unload,
    python bridge.py module reload gamepad          # reload (after a rebuild)

Maneuver files are per project (PS2_INPUT_MANEUVERS names a default). CSV,
'#' starts a comment line; one row per step, and rows sharing a name run in
order:

    name,seconds,buttons,lx,ly,rx,ry,l2,r2,note
    tap-start,0.15,start,,,,,,,
    roll-left,0.08,,1,,,,,,arm with a right slam
    roll-left,0.08,,-1,,,,,,left slam launches

Empty cells are neutral. Axes run -1..1 (x left..right, y up..down), l2/r2
0..1 (a listed l2/r2 button alone means 1). Buttons are joined with '+':
select l3 r3 start up right down left l2 r2 l1 r1 triangle circle cross
square. A buttons cell of '@other' plays the maneuver 'other' in place.

A state is held by resending it at 60 Hz; the bridge releases every control
500 ms after the last datagram. The datagram layout is documented in
bridge.c.
"""
import argparse
import csv
import math
import os
import socket
import struct
import sys
import time

PORT = int(os.environ.get("INPUT_BRIDGE_PORT", 0x4716))
CONTROL_PORT = int(os.environ.get("INPUT_CONTROL_PORT", 0x4717))
RATE_HZ = 60

BUTTONS = {
    "select": 0x0001, "l3": 0x0002, "r3": 0x0004, "start": 0x0008,
    "up": 0x0010, "right": 0x0020, "down": 0x0040, "left": 0x0080,
    "l2": 0x0100, "r2": 0x0200, "l1": 0x0400, "r1": 0x0800,
    "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000,
}
AXES = ("lx", "ly", "rx", "ry")


def axis(v):
    """-1.0 .. 1.0 (left/up .. right/down) to the pad byte, 0x80 = centre."""
    return max(0, min(255, round(128 + v * 127.5)))


def trigger(v):
    return max(0, min(255, round(v * 255)))


def packet(buttons=(), lx=0.0, ly=0.0, rx=0.0, ry=0.0, l2=None, r2=None):
    bits = 0
    for name in buttons:
        bits |= BUTTONS[name]
    l2 = (1.0 if "l2" in buttons else 0.0) if l2 is None else l2
    r2 = (1.0 if "r2" in buttons else 0.0) if r2 is None else r2
    return struct.pack("<4sBBH6B", b"PKBR", 1, 0, bits,
                       axis(lx), axis(ly), axis(rx), axis(ry), trigger(l2), trigger(r2))


class Bridge:
    def __init__(self, port=PORT):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addr = ("127.0.0.1", port)

    def hold(self, seconds, state=None, **kw):
        """Sends one state, or state(t) for t in 0..1, for `seconds`."""
        start = time.perf_counter()
        while True:
            t = (time.perf_counter() - start) / seconds
            if t >= 1.0:
                return
            self.sock.sendto(state(t) if state else packet(**kw), self.addr)
            time.sleep(1.0 / RATE_HZ)

    def release(self, seconds=0.2):
        self.hold(seconds)


def control(command, port=CONTROL_PORT, timeout=2.0):
    """Sends one command to ps2client's input control port; returns its reply."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    sock.sendto(command.encode(), ("127.0.0.1", port))
    try:
        reply = sock.recv(4096).decode(errors="replace")
    except (socket.timeout, ConnectionResetError):
        raise SystemExit(f"no reply from ps2client's input control port {port} (is it streaming with --input?)")
    return reply if reply.endswith("\n") else reply + "\n"


def load_maneuvers(path):
    """{name: [step dict, ...]} in file order."""
    with open(path, newline="", encoding="utf-8") as f:
        rows = [line for line in f if line.strip() and not line.lstrip().startswith("#")]
    maneuvers = {}
    for n, row in enumerate(csv.DictReader(rows), 1):
        def cell(key):
            return (row.get(key) or "").strip()
        buttons = [b for b in cell("buttons").replace("+", " ").split() if b]
        step = {"name": cell("name"), "note": cell("note")}
        if buttons and buttons[0].startswith("@"):
            step["include"] = buttons[0][1:]
        else:
            for b in buttons:
                if b not in BUTTONS:
                    raise SystemExit(f"{path}: step {n}: unknown button '{b}'")
            step["buttons"] = buttons
            step["seconds"] = float(cell("seconds") or 0)
            for key in AXES:
                step[key] = float(cell(key) or 0)
            for key in ("l2", "r2"):
                step[key] = float(cell(key)) if cell(key) else None
        maneuvers.setdefault(step["name"], []).append(step)
    return maneuvers


def run(bridge, maneuvers, name, depth=0):
    if name not in maneuvers:
        raise SystemExit(f"unknown maneuver '{name}' (try: list)")
    if depth > 16:
        raise SystemExit(f"maneuver '{name}' includes itself")
    for step in maneuvers[name]:
        if "include" in step:
            run(bridge, maneuvers, step["include"], depth + 1)
            continue
        if step["note"]:
            print(f"  {name}: {step['note']}", flush=True)
        bridge.hold(step["seconds"], buttons=step["buttons"],
                    **{k: step[k] for k in AXES + ("l2", "r2")})


def tour(b):
    """Every control once, for checking a program's input path."""
    steps = [
        ("tap START", lambda: b.hold(0.15, buttons=["start"])),
        ("release", b.release),
        ("left stick: one full circle", lambda: b.hold(3.0, lambda t: packet(
            lx=math.cos(t * 2 * math.pi), ly=math.sin(t * 2 * math.pi)))),
        ("face buttons", lambda: [b.hold(0.4, buttons=[n]) for n in ("cross", "circle", "square", "triangle")]),
        ("L1 + R1", lambda: b.hold(1.0, buttons=["l1", "r1"])),
        ("analog L2 ramp", lambda: b.hold(1.5, lambda t: packet(l2=t))),
        ("D-pad up, right, down, left", lambda: [b.hold(0.4, buttons=[d]) for d in ("up", "right", "down", "left")]),
        ("right stick sweep", lambda: b.hold(2.0, lambda t: packet(rx=math.sin(t * 2 * math.pi)))),
        ("L3 + R3", lambda: b.hold(0.4, buttons=["l3", "r3"])),
        ("release", b.release),
    ]
    for name, step in steps:
        print(name, flush=True)
        step()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", type=int, default=PORT, help="bridge port (INPUT_BRIDGE_PORT, default 18198)")
    p.add_argument("--control-port", type=int, default=CONTROL_PORT,
                   help="client control port (INPUT_CONTROL_PORT, default 18199)")
    p.add_argument("--file", default=os.environ.get("PS2_INPUT_MANEUVERS"),
                   help="maneuver file (PS2_INPUT_MANEUVERS)")
    sub = p.add_subparsers(dest="command", required=True)
    press = sub.add_parser("press", help="hold buttons and axes, then release")
    press.add_argument("buttons", nargs="*", metavar="button")
    for key in AXES:
        press.add_argument("--" + key, type=float, default=0.0)
    press.add_argument("--l2", type=float)
    press.add_argument("--r2", type=float)
    press.add_argument("--for", dest="seconds", type=float, default=0.15, help="seconds to hold")
    runp = sub.add_parser("run", help="play maneuvers from --file in order")
    runp.add_argument("names", nargs="+")
    runp.add_argument("--repeat", type=int, default=1)
    sub.add_parser("list", help="list the maneuvers in --file")
    sub.add_parser("tour", help="exercise every control once")
    mod = sub.add_parser("module", help="hot-swap ps2client's input modules")
    mod.add_argument("action", choices=("list", "load", "unload", "reload"))
    mod.add_argument("name", nargs="?", help="module name or path")
    a = p.parse_args()

    if a.command == "module":
        if a.action != "list" and not a.name:
            p.error(f"module {a.action} needs a module name")
        print(control(" ".join(filter(None, (a.action, a.name))), a.control_port), end="")
        return

    if a.command in ("run", "list"):
        if not a.file:
            p.error("run/list need --file or PS2_INPUT_MANEUVERS")
        maneuvers = load_maneuvers(a.file)
        if a.command == "list":
            def seconds(name, depth=0):
                if name not in maneuvers or depth > 16:
                    return 0.0
                return sum(seconds(s["include"], depth + 1) if "include" in s else s["seconds"]
                           for s in maneuvers[name])
            for name, steps in maneuvers.items():
                total = seconds(name)
                notes = "; ".join(s["note"] or "@" + s["include"] if "include" in s else s["note"]
                                  for s in steps if s["note"] or "include" in s)
                print(f"{name:20} {total:5.2f} s  {notes}")
            return

    b = Bridge(a.port)
    if a.command == "press":
        for name in a.buttons:
            if name not in BUTTONS:
                p.error(f"unknown button '{name}'")
        b.hold(a.seconds, buttons=a.buttons, lx=a.lx, ly=a.ly, rx=a.rx, ry=a.ry, l2=a.l2, r2=a.r2)
        b.release()
    elif a.command == "run":
        for _ in range(a.repeat):
            for name in a.names:
                print(name, flush=True)
                run(b, maneuvers, name)
        b.release()
    else:
        tour(b)


if __name__ == "__main__":
    sys.exit(main())
