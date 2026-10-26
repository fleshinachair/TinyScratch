#!/usr/bin/env python3
"""Downloads long, famous, *public-domain* recordings for auditioning TinyScratch.

The classic battle-record cuts ("Ahhh", "Fresh", ...) are copyrighted, so this
fetches material that is free to use instead:

  * NASA mission audio - works of the US government, public domain.
  * 1925 78s from the Internet Archive's Great 78 Project - US sound recordings
    published in 1925 entered the public domain on 1 January 2026.

Files land in samples_audition/ (gitignored) as mono 16-bit 44.1 kHz WAVs, with
SOURCES.txt recording where each came from. scripts/install_move.sh pushes
them to the Move. Run through scripts/fetch_audition_samples.sh, which builds
the converter first.

    python tools/fetch_audition_samples.py --converter build/audio_convert[.exe]
"""
import argparse
import os
import subprocess
import sys
import urllib.parse
import urllib.request

NASA = "https://nssdc.gsfc.nasa.gov"
G78 = "https://archive.org/download"
YELLOW = (G78 + "/78_yellow-dog-blues_bessie-smith-hendersons-hot-six_gbia0464774a/"
          + urllib.parse.quote("Yellow Dog Blues - BESSIE SMITH - Henderson's Hot Six.flac"))
MYMAN = (G78 + "/78_my-man-blues_bessie-clara-smith_gbia0465410b/"
         + urllib.parse.quote("My Man Blues - BESSIE & CLARA SMITH.flac"))

PD_NASA = "Public domain (NASA, work of the US government)"
PD_1925 = "Public domain in the US (published 1925); Great 78 Project transfer, Internet Archive"

# output name, url, start s, length s (0 = whole), what it is, rights
SOURCES = []                                 # (none at the moment: add your own public-domain sources here)


def download(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return
    req = urllib.request.Request(url, headers={"User-Agent": "TinyScratch-audition-fetch/1.0"})
    tmp = dest + ".part"
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
    os.replace(tmp, dest)


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, ".."))
    ap = argparse.ArgumentParser(description="Fetch public-domain audition samples for TinyScratch.")
    ap.add_argument("--converter", required=True, help="path to the built audio_convert tool")
    ap.add_argument("--out", default=os.path.join(root, "samples_audition"))
    ap.add_argument("--cache", default=os.path.join(root, "build", "audition_cache"))
    args = ap.parse_args(argv)

    args.converter = os.path.abspath(args.converter)   # subprocess on Windows will not resolve it relative
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.cache, exist_ok=True)
    lines = ["TinyScratch audition samples - not part of the module, not committed.", ""]
    failed = 0
    for name, url, start, length, what, rights in SOURCES:
        ext = os.path.splitext(urllib.parse.urlparse(url).path)[1].lower()
        cached = os.path.join(args.cache, urllib.parse.unquote(os.path.basename(urllib.parse.urlparse(url).path)))
        out = os.path.join(args.out, name + ".wav")
        try:
            download(url, cached)
            cmd = [args.converter, cached, out]
            if start or length:
                cmd += ["--start", str(start), "--len", str(length)]
            subprocess.run(cmd, check=True)
        except Exception as e:                       # report and carry on with the rest
            print("FAILED %s: %s" % (name, e), file=sys.stderr)
            failed += 1
            continue
        excerpt = " [%d-%d s]" % (start, start + length) if length else ""
        lines.append("%s.wav  -  %s%s\n    %s\n    source: %s" % (name, what, excerpt, rights, url))
    with open(os.path.join(args.out, "SOURCES.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("%d of %d samples in %s" % (len(SOURCES) - failed, len(SOURCES), args.out))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
