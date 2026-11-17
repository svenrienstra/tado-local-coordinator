"""Timing of wake-up trains, Data frames and Enhanced ACKs in a SniffBench capture.

Usage: analyze_sniff.py <capture>


Line format: <rat_end> <rssi> <U|R|L> <hex or text>. rat_end is the RAT (4 MHz) time the
frame ended on air. A frame's on-air start is rat_end - (preamble 4 + sync 4 + len 1 + n + CRC 2)
bytes at 640 RAT a byte (assumes the sender's preamble matches ours; see notes).
"""
import statistics
import sys

RAT_PER_BYTE = 640
RAT_PER_US = 4


def start_of(rat_end, n):
    return rat_end - (4 + 4 + 1 + n + 2) * RAT_PER_BYTE


def us(rat):
    return rat / RAT_PER_US


frames = []
for line in open(sys.argv[1]):
    parts = line.split()
    if len(parts) < 4 or parts[2] != "U":
        continue
    frames.append((int(parts[0]), int(parts[1]), bytes.fromhex(parts[3])))

wake = lambda f: len(f) == 12 and f[0] == 0x25 and f[6:8] == b"\x82\x0e"
ack = lambda f: len(f) == 29 and f[0] == 0x42 and f[1] == 0xEE

# 1. Trains: runs of wake-up frames with one seq and destination, and the frame after.
trains = []
cur = None
for i, (rat, rssi, f) in enumerate(frames):
    if wake(f):
        key = (f[1], f[4:6])
        if cur and cur["key"] == key and rat - cur["frames"][-1][0] < 50_000 * RAT_PER_US:
            cur["frames"].append((rat, int.from_bytes(f[8:10], "little")))
        else:
            cur = {"key": key, "frames": [(rat, int.from_bytes(f[8:10], "little"))], "next": None}
            trains.append(cur)
    elif cur and cur["next"] is None and len(f) > 12 and f[2] == cur["key"][0]:
        cur["next"] = (rat, f)

print(f"{len(frames)} frames, {len(trains)} trains")
for t in trains:
    fr = t["frames"]
    steps = [us(b[0] - a[0]) for a, b in zip(fr, fr[1:])]
    line = (f"train seq={t['key'][0]:02x} dst={t['key'][1].hex()} frames={len(fr)} "
            f"R {fr[0][1]}->{fr[-1][1]}")
    if steps:
        line += f" step median {statistics.median(steps):.0f} us (min {min(steps):.0f}, max {max(steps):.0f})"
    if t["next"]:
        rat, f = t["next"]
        data_start = start_of(rat, len(f))
        last_end = fr[-1][0]
        line += f"\n    data len {len(f)} fcf {f[:2].hex()} starts {us(data_start - last_end):.0f} us after the last wake frame ends"
        # Announced vs actual: rendezvous R (320 us units) counted from each wake frame's end.
        errs = [us(data_start - end) - r * 320 for end, r in fr]
        line += (f"\n    data start minus announced (R*320us from frame end): "
                 f"median {statistics.median(errs):.0f} us, min {min(errs):.0f}, max {max(errs):.0f}")
    print(line)

# 2. Enhanced ACKs: gap from the end of the acked frame (same seq, just before) to the ACK start.
print()
gaps = []
for i, (rat, rssi, f) in enumerate(frames):
    if not ack(f):
        continue
    for prev_rat, _, p in reversed(frames[max(0, i - 4):i]):
        if len(p) > 12 and not ack(p) and p[2] == f[2]:
            gap = us(start_of(rat, len(f)) - prev_rat)
            gaps.append(gap)
            print(f"ack seq={f[2]:02x} to {f[3:11].hex()} from {f[11:19].hex()}: "
                  f"starts {gap:.0f} us after the frame ends; phase {int.from_bytes(f[21:23], 'little')}")
            break
if gaps:
    print(f"ACK gap median {statistics.median(gaps):.0f} us, min {min(gaps):.0f}, max {max(gaps):.0f} "
          f"({len(gaps)} ACKs)")
