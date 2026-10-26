#!/usr/bin/env python3
"""Reads clips (playhead automation and notes) out of an Ableton Live Pack you own and writes local/clips.bin, which
TinyScratch plays instead of its built-in clips.

    python tools/import_clips.py "path/to/your pack.alp"

The clips are someone else's work: they are read from the copy you have, kept in local/ (not committed), and installed to
your own Move by scripts/install_move.sh. Nothing from the pack is stored in the repository.

What it takes from each MIDI clip that has playhead (Turntable) automation: its length in beats (4 to 16), that
automation (drawn with Bezier curves in Live: the curves are evaluated here) and the notes. Both are written out densely,
128 points to the beat, so the DSP needs no curve maths. A clip whose name starts with "Ahhh" is tagged for the Ahhh
sample, one that starts with "Fresssh" for Fresssh, any other for any sample.

local/clips.bin
    "TSCL"  u8 version (1)  u8 clip count
    per clip:  u8 tag (1 = made for Ahhh, 2 = for Fresssh, 0 = either)  u8 length in beats  u16 n (= length * 128)
               n x u16 playhead position (0..65535)    n x u8 cut (0 or 1)
"""
import gzip
import os
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

RES = 128                                   # points to the beat
MAX_BEATS = 16


def load_set(path):
    raw = gzip.open(path).read()            # the pack is gzip, holding the Live Set as a second gzip stream
    i = raw.find(b"\x1f\x8b\x08\x00")
    if i < 0:
        raise SystemExit("no Live Set found in " + path)
    xml = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(raw[i:])
    return ET.fromstring(xml)


def val(e, tag, attr="Value"):
    x = e.find(tag)
    return x.get(attr) if x is not None else None


def bezier_y(u, c1x, c1y, c2x, c2y):
    """y of the cubic Bezier (0,0) (c1) (c2) (1,1) where its x is u."""
    lo, hi = 0.0, 1.0
    for _ in range(40):
        s = 0.5 * (lo + hi)
        x = 3 * (1 - s) ** 2 * s * c1x + 3 * (1 - s) * s * s * c2x + s ** 3
        if x < u:
            lo = s
        else:
            hi = s
    s = 0.5 * (lo + hi)
    return 3 * (1 - s) ** 2 * s * c1y + 3 * (1 - s) * s * s * c2y + s ** 3


def curve_value(events, t):
    """events: [(time, value, curve or None)] sorted by time (a repeated time is a jump: the later one wins)."""
    idx = -1
    for k, ev in enumerate(events):
        if ev[0] <= t:
            idx = k
        else:
            break
    if idx < 0:
        return events[0][1]
    t0, v0, cv = events[idx]
    if idx + 1 >= len(events):
        return v0
    t1, v1, _ = events[idx + 1]
    if t1 <= t0:
        return v1
    u = (t - t0) / (t1 - t0)
    y = bezier_y(u, *cv) if cv else u
    return v0 + (v1 - v0) * y


def read_clips(root):
    clips = []
    for mc in root.iter("MidiClip"):
        name = val(mc, "Name") or ""
        loop = mc.find("Loop")
        length = float(val(loop, "LoopEnd")) - float(val(loop, "LoopStart")) if loop is not None else 0.0
        turn = None
        for env in mc.iter("ClipEnvelope"):
            evs = []
            for f in env.iter("FloatEvent"):
                t = float(f.get("Time"))
                if t < -1e6:
                    continue                                 # Live's "before everything" placeholder
                cv = None
                if f.get("CurveControl1X") is not None:
                    cv = tuple(float(f.get(k)) for k in ("CurveControl1X", "CurveControl1Y", "CurveControl2X", "CurveControl2Y"))
                evs.append((t, float(f.get("Value")), cv))
            if len(evs) > 2:                                 # the Turntable: many points (the other is a constant, the preset)
                turn = evs
        notes = []
        for kt in mc.iter("KeyTrack"):
            for n in kt.iter("MidiNoteEvent"):
                notes.append((float(n.get("Time")), float(n.get("Duration"))))
        if turn is None or not notes or not (4 <= length <= MAX_BEATS) or length != int(length):
            continue
        tag = 1 if name.lower().startswith("ahhh") else (2 if name.lower().startswith("fresssh") else 0)
        clips.append({"name": name, "tag": tag, "len": int(length), "turn": sorted(turn, key=lambda e: e[0]), "notes": notes})
    return clips


def dense(c):
    n = c["len"] * RES
    pos, gate = [], [0] * n
    for i in range(n):
        v = curve_value(c["turn"], i / RES)
        pos.append(max(0, min(65535, int(round(v * 65535)))))
    for start, dur in c["notes"]:
        a = max(0, int(start * RES + 0.5))
        b = min(n, max(a + 1, int((start + dur) * RES + 0.5)))
        for i in range(a, b):
            gate[i] = 1
    return pos, gate


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    clips = read_clips(load_set(sys.argv[1]))
    if not clips:
        raise SystemExit("no clips with playhead automation and notes found")
    clips = clips[:8]
    out = bytearray(b"TSCL" + bytes([1, len(clips)]))
    for c in clips:
        pos, gate = dense(c)
        out += struct.pack("<BBH", c["tag"], c["len"], len(pos))
        out += struct.pack("<%dH" % len(pos), *pos)
        out += bytes(gate)
        opened = sum(gate) * 100 // len(gate)
        print("%-14s %2d beats  tag %d  %2d notes  cut in %2d%% of the time  playhead %.2f..%.2f" % (
            c["name"], c["len"], c["tag"], len(c["notes"]), opened, min(pos) / 65535, max(pos) / 65535))
    dest = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "local")
    os.makedirs(dest, exist_ok=True)
    path = os.path.join(dest, "clips.bin")
    with open(path, "wb") as f:
        f.write(out)
    print("wrote %s (%d bytes, %d clips)" % (os.path.normpath(path), len(out), len(clips)))


if __name__ == "__main__":
    main()
