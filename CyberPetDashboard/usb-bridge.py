#!/usr/bin/env python3
"""USB sync bridge for the CyberPet device.

The device prefers USB over WiFi for dashboard sync when a host has its CDC
serial port open. It sends one line per sync attempt:

    SYNC {"deviceId":"...","petState":{...},"completedHabits":[...]}

This script relays that to the dashboard's POST /api/sync and writes the JSON
response back as:

    SYNCRESP {...}

Every other line from the device is an ordinary log line and is echoed to
stdout — so this doubles as a serial monitor. Run this INSTEAD of
`cat /dev/ttyACM0` (two readers on one tty split the byte stream).

No third-party dependencies; stdlib only.

Usage:
    python3 usb-bridge.py [port] [dashboard-url]
Defaults:
    port          /dev/ttyACM0
    dashboard-url http://localhost:8090
"""
import os
import sys
import time
import tty
import urllib.error
import urllib.request

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
base = (sys.argv[2] if len(sys.argv) > 2 else "http://localhost:8090").rstrip("/")


def open_port():
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    tty.setraw(fd)  # no echo, no line editing, binary-safe
    return fd


def write_chunked(fd, data, chunk=256, gap=0.003):
    # Small chunks with gaps so the device's CDC RX buffer can't overflow
    # even if the firmware is between reads.
    for i in range(0, len(data), chunk):
        os.write(fd, data[i : i + chunk])
        time.sleep(gap)


def handle_sync(fd, body):
    try:
        req = urllib.request.Request(
            base + "/api/sync",
            data=body,
            headers={"Content-Type": "application/json"},
        )
        resp = urllib.request.urlopen(req, timeout=5).read()
    except (urllib.error.URLError, OSError) as e:
        print(f"usb-bridge: dashboard unreachable: {e}", flush=True)
        return
    write_chunked(fd, b"SYNCRESP " + resp + b"\n")
    print(f"usb-bridge: synced ({len(resp)} bytes)", flush=True)


print(f"usb-bridge: {port} <-> {base}/api/sync  (Ctrl-C to quit)", flush=True)
while True:
    try:
        fd = open_port()
    except OSError as e:
        print(f"usb-bridge: cannot open {port}: {e}; retrying in 2 s", flush=True)
        time.sleep(2)
        continue

    buf = b""
    try:
        while True:
            data = os.read(fd, 4096)
            if not data:
                raise OSError("EOF")
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.rstrip(b"\r")
                if line.startswith(b"SYNC "):
                    handle_sync(fd, line[5:])
                elif line:
                    print(line.decode("utf-8", "replace"), flush=True)
    except OSError:
        print("usb-bridge: port lost (device unplugged?); reopening in 2 s", flush=True)
        try:
            os.close(fd)
        except OSError:
            pass
        time.sleep(2)
    except KeyboardInterrupt:
        print("\nusb-bridge: bye", flush=True)
        try:
            os.close(fd)
        except OSError:
            pass
        sys.exit(0)
