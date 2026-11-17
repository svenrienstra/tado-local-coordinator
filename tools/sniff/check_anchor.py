"""Check the CSL anchor maths on a bridge capture (SniffBench format).

Usage: check_anchor.py <capture> <bridge EUI, 16 hex digits>


For each device -> bridge Enhanced ACK (phase P, end E), the device sampled at
S = E - P * 320 us, and again every CSL period (1 s). Each later short train from
the bridge to that device should be centred on such a sample. Also: the reply
latency from a device uplink to the bridge's next train for it.
"""
import statistics
import sys

RAT_US = 4
PERIOD = 1_000_000 * RAT_US
WRAP = 1 << 32
BYTE = 640


def start_of(rat_end, n):
    return rat_end - (4 + 4 + 1 + n + 2) * BYTE


frames = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) >= 4 and p[2] == "U":
        frames.append((int(p[0]), bytes.fromhex(p[3])))

# Unwrap the 32-bit RAT into a monotonic 64-bit time.
t, base, prev = [], 0, None
for rat, f in frames:
    if prev is not None and rat < prev and prev - rat > WRAP // 2:
        base += WRAP
    prev = rat
    t.append((base + rat, f))

BRIDGE = bytes.fromhex(sys.argv[2])
anchors = {}  # short -> list of (E, S)
for rat, f in t:
    if len(f) == 29 and f[:2] == b"\x42\xee" and f[3:11] == BRIDGE:
        src = f[11:19]
        phase = int.from_bytes(f[21:23], "little")
        anchors.setdefault(bytes([src[1], src[0]]), []).append((rat, rat - phase * 1280))

print("anchors per device (short: count, spread of S mod 1 s in us):")
for short, lst in anchors.items():
    mods = [s % PERIOD for _, s in lst]
    ref = mods[0]
    offs = [((m - ref + PERIOD // 2) % PERIOD - PERIOD // 2) / RAT_US for m in mods]
    print(f"  {short.hex()}: {len(lst)} ACKs, S mod 1 s varies {min(offs):.0f}..{max(offs):.0f} us")

# Short trains: runs of wake frames to one short address.
trains, cur = [], None
for rat, f in t:
    if len(f) == 12 and f[0] == 0x25 and f[6:8] == b"\x82\x0e":
        key = (f[1], f[4:6])
        if cur and cur["key"] == key and rat - cur["ends"][-1] < 50_000 * RAT_US:
            cur["ends"].append(rat)
            cur["r"].append(int.from_bytes(f[8:10], "little"))
        else:
            cur = {"key": key, "ends": [rat], "r": [int.from_bytes(f[8:10], "little")]}
            trains.append(cur)

print("\nshort trains vs the latest earlier anchor of their device (offset of train centre from a sample):")
offsets = []
for tr in trains:
    if len(tr["ends"]) > 30 or tr["key"][1] == b"\xff\xff":
        continue
    short = tr["key"][1]
    first_start = start_of(tr["ends"][0], 12)
    last_end = tr["ends"][-1]
    centre = (first_start + last_end) // 2
    prior = [s for e, s in anchors.get(short, []) if e < first_start]
    if not prior:
        continue
    s = prior[-1]
    off = ((centre - s + PERIOD // 2) % PERIOD - PERIOD // 2) / RAT_US
    offsets.append(off)
    print(f"  {short.hex()} {len(tr['ends'])} frames R {tr['r'][0]}->{tr['r'][-1]}: centre {off:+.0f} us from sample")
if offsets:
    print(f"  median {statistics.median(offsets):+.0f} us, min {min(offsets):+.0f}, max {max(offsets):+.0f} ({len(offsets)} trains)")

# R values of one short train (golden vector).
for tr in trains:
    if len(tr["ends"]) == 21:
        print("\nR values of a 21-frame train:", tr["r"])
        break

# Reply latency: device unicast uplink to the bridge -> start of the bridge's next train to it.
print("\nreply latency (device uplink end -> next train to it, first frame start), ms:")
lat = []
for i, (rat, f) in enumerate(t):
    if len(f) > 29 and f[0] in (0x69, 0x79) and f[3:11] == BRIDGE:
        src5 = f[11:16]
        short = bytes([src5[1], src5[0]])
        for tr in trains:
            st = start_of(tr["ends"][0], 12)
            if tr["key"][1] == short and st > rat:
                lat.append((st - rat) / RAT_US / 1000)
                break
if lat:
    lat.sort()
    print(f"  n {len(lat)}, min {lat[0]:.0f}, median {statistics.median(lat):.0f}, max {lat[-1]:.0f}")
    print("  ", [round(x) for x in lat[:40]])
