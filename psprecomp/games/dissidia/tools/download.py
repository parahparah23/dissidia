#!/usr/bin/env python3
"""Download files for an install extra (setup/extras.cpp ExtraCtx::download on the desktop; the Android app downloads them itself).

  download.py URL FILE [URL FILE ...]

A file that is already there is kept (a retried install continues). Each download goes through FILE.part and is renamed when complete.
Prints "[update] PROGRESS <percent>" lines (the wizard's progress bar) and "[update] <message>" lines. Standard library only.
"""
import os
import sys
import urllib.request

UA = {"User-Agent": "psprecomp-setup"}


def fetch(url, dest, index, count):
    if os.path.isfile(dest):
        print("[update] already downloaded: %s" % os.path.basename(dest), flush=True)
        return
    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
    part = dest + ".part"
    print("[update] downloading %s" % url, flush=True)
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r, open(part, "wb") as out:
        total = int(r.headers.get("Content-Length") or 0)
        done, last = 0, -1
        while True:
            b = r.read(1 << 16)
            if not b:
                break
            out.write(b)
            done += len(b)
            pct = int(100 * (index + (done / total if total else 0)) / count)
            if pct != last:
                print("[update] PROGRESS %d" % pct, flush=True)
                last = pct
    os.replace(part, dest)


def main():
    a = sys.argv[1:]
    if not a or len(a) % 2:
        sys.exit(__doc__)
    pairs = list(zip(a[0::2], a[1::2]))
    try:
        for i, (url, dest) in enumerate(pairs):
            fetch(url, dest, i, len(pairs))
    except Exception as e:  # noqa: BLE001 - reported to the wizard
        print("[update] ERROR the download failed (%s)" % e, flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
