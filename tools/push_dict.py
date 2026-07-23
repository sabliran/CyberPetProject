#!/usr/bin/env python3
"""push_dict.py — send tools/dict_out to the device's SD card over WiFi.

Uploads words.idx + defs.dat to the dashboard (POST /api/dict/files/<name>),
then POST /api/dict/publish, which stamps a size+MD5 manifest and bumps the
store's dictPushToken. The next device sync sees the new token (the config
bump makes that near-immediate on a live device), shows "Updating
dictionary...", pulls both files over WiFi HTTP, verifies them against the
manifest, and swaps them in on the card — no TF-card shuffling.

Typical flow after changing the dictionary format or content:
  tools/.venv-dict/bin/python tools/make_dict.py
  python3 tools/push_dict.py            # stdlib only, any python3

DASHBOARD_URL env overrides the default http://localhost:8090.
Watch progress in the dashboard bridge log (the device prints
"dict update: ..." lines over USB serial).
"""

import hashlib
import json
import os
import sys
import urllib.error
import urllib.request

BASE = os.environ.get("DASHBOARD_URL", "http://localhost:8090")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dict_out")
FILES = ["words.idx", "defs.dat"]


def post(path, data, content_type):
    req = urllib.request.Request(BASE + path, data=data, method="POST",
                                 headers={"Content-Type": content_type})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return json.load(resp)


def main():
    for name in FILES:
        p = os.path.join(OUT_DIR, name)
        if not os.path.exists(p):
            sys.exit(f"FATAL: {p} missing - run tools/make_dict.py first")

    for name in FILES:
        with open(os.path.join(OUT_DIR, name), "rb") as f:
            data = f.read()
        resp = post(f"/api/dict/files/{name}", data, "application/octet-stream")
        print(f"uploaded {name}: {resp['bytes']:,} bytes "
              f"(md5 {hashlib.md5(data).hexdigest()})")

    resp = post("/api/dict/publish", b"", "application/json")
    print(f"\npublished as token {resp['token']}:")
    for f in resp["files"]:
        print(f"  {f['name']}: {f['size']:,} bytes, md5 {f['md5']}")
    print("\nThe device applies it on its next sync (seconds when live; the"
          "\ndownload itself takes about a minute - watch the bridge log).")


if __name__ == "__main__":
    try:
        main()
    except urllib.error.URLError as e:
        sys.exit(f"FATAL: dashboard at {BASE} unreachable ({e})")
