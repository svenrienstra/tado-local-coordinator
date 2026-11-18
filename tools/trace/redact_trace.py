#!/usr/bin/env python3
"""Decode host-link captures of one device into a redacted text trace.

Reads host/state/captures/hostlink-*.log (written by the Java host) and prints
the device's traffic with the coordinator: 6LoWPAN fragments reassembled,
ICMPv6 by type, CoAP with its options, and payloads as tado TLVs (2-byte tag,
1-byte length, big-endian value), Block2 bodies joined again.

Only frames the CC1312 opened (RX) and the host's TX to the device are kept;
the frames of other plants nearby (RX_UNVERIFIED) are dropped. Every real
identifier is replaced by an invented one: both EUIs in every form they take
(hex, reversed, the IPv6 interface id, its text form), the serial, the home id
(text and u32). The d/pair key is never printed, auth/token session ids and
the session option are replaced. The run fails if any real value, or a
3-byte piece of one, is left in the output.

    python3 tools/trace/redact_trace.py --state host/state \\
        --device <EUI> --serial <serial> capture.log... > trace.txt
"""

import argparse
import re
import sys

# Invented values with the length of the real ones in every form, so a redacted
# body keeps its TLV lengths.
FAKE_DEVICE = "1a2b3c3107c51b00"
FAKE_COORDINATOR = "a1b2c37520c51b00"
FAKE_SERIAL = "SU1234567890"
FAKE_HOME = 123456789

OPTION_NAMES = {1: "If-Match", 4: "ETag", 6: "Observe", 7: "Uri-Port", 11: "Uri-Path", 12: "CF",
                14: "Max-Age", 15: "Uri-Query", 17: "Accept", 23: "Block2", 27: "Block1",
                28: "Size2", 60: "Size1", 2048: "Session"}
TYPES = ["CON", "NON", "ACK", "RST"]
METHODS = {1: "GET", 2: "POST", 3: "PUT", 4: "DELETE"}
ICMP = {128: "Echo Request", 129: "Echo Reply", 133: "Router Solicitation", 134: "Router Advertisement", 135: "Neighbor Solicitation",
        136: "Neighbor Advertisement"}
LINE = re.compile(r"^(\S+) ([<>]) seq=\S+ (\S+)(?: \(key redacted\))? ?(\S*)$")


def read_properties(path):
    props = {}
    with open(path) as f:
        for line in f:
            if "=" in line and not line.startswith("#"):
                k, v = line.strip().split("=", 1)
                props[k] = v
    return props


def iid(eui):
    """The IPv6 interface id of an EUI held little-endian, as the devices build it."""
    b = bytearray(bytes.fromhex(eui)[::-1])
    b[0] ^= 0x02
    return b.hex()


def iid_text(eui):
    h = iid(eui)
    return ":".join(format(int(h[i:i + 4], 16), "x") for i in range(0, 16, 4))


class Redactor:
    def __init__(self, device, coordinator, serial, home, key):
        # (real, invented, whether it is hex for bytes)
        self.pairs = []
        for real, fake in ((device, FAKE_DEVICE), (coordinator, FAKE_COORDINATOR)):
            self.pairs += [(real, fake, True),
                           (bytes.fromhex(real)[::-1].hex(), bytes.fromhex(fake)[::-1].hex(), True),
                           (iid(real), iid(fake), True), (iid_text(real), iid_text(fake), False)]
        self.pairs += [(serial, FAKE_SERIAL, False), (str(home), str(FAKE_HOME), False),
                       (format(home, "08x"), format(FAKE_HOME, "08x"), True)]
        for real, fake, _ in self.pairs:
            if len(real) != len(fake):
                raise ValueError("an invented value must have the length of the real one")
        self.pairs.sort(key=lambda p: -len(p[0]))
        # Pieces that must not survive: every 3-byte window of each real value,
        # as hex and as the hex of its text, and the text itself.
        self.secrets = {key} if key else set()
        common = set()
        for real, fake, is_hex in self.pairs:
            for value, out in ((real, self.secrets), (fake, common)):
                forms = [value.encode().hex()]
                if is_hex:
                    forms.append(value)
                else:
                    out.add(value)
                for h in forms:
                    out |= {h[i:i + 6] for i in range(0, len(h) - 5, 2)}
        # A window shared with the invented values is not identifying (the OUI).
        self.secrets -= common

    def __call__(self, text):
        for real, fake, _ in self.pairs:
            text = text.replace(real, fake)
        return text

    def body(self, b):
        """The same replacements in a payload, on its bytes and on its text."""
        for real, fake, is_hex in self.pairs:
            b = b.replace(real.encode(), fake.encode())
            if is_hex:
                b = b.replace(bytes.fromhex(real), bytes.fromhex(fake))
        return b

    def leaks(self, text):
        return sorted(s for s in self.secrets if s and s in text.lower())


class Reader:
    def __init__(self, b):
        self.b, self.i = b, 0

    def take(self, n):
        if self.i + n > len(self.b):
            raise ValueError("short")
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return int.from_bytes(self.take(2), "big")

    def rest(self):
        return self.b[self.i:]


def skip_address(r, stateful, multicast, mode):
    if multicast and stateful:
        r.take(6)
    elif multicast:
        r.take([16, 6, 4, 1][mode])
    elif stateful:
        r.take([0, 8, 2, 0][mode])
    else:
        r.take([16, 8, 2, 0][mode])


def iphc(packet):
    """Returns (kind, ports, payload): kind is 'udp' or an ICMPv6 type."""
    r = Reader(packet)
    b0, b1 = r.u8(), r.u8()
    if b0 & 0xE0 != 0x60:
        raise ValueError("not IPHC")
    if b1 & 0x80:
        r.take(1)
    r.take([4, 3, 1, 0][(b0 >> 3) & 3])
    nh_compressed = b0 & 0x04
    nh = None if nh_compressed else r.u8()
    if b0 & 3 == 0:
        r.take(1)
    skip_address(r, b1 & 0x40, False, (b1 >> 4) & 3)
    skip_address(r, b1 & 0x04, b1 & 0x08, b1 & 3)
    if nh_compressed:
        nhc = r.u8()
        if nhc & 0xF8 != 0xF0:
            raise ValueError("NHC")
        m = nhc & 3
        if m == 0:
            sp, dp = r.u16(), r.u16()
        elif m == 1:
            sp, dp = r.u16(), 0xF000 | r.u8()
        elif m == 2:
            sp, dp = 0xF000 | r.u8(), r.u16()
        else:
            x = r.u8()
            sp, dp = 0xF0B0 | x >> 4, 0xF0B0 | x & 15
        if not nhc & 0x04:
            r.take(2)
        return "udp", (sp, dp), r.rest()
    if nh == 17:
        sp, dp = r.u16(), r.u16()
        r.take(4)
        return "udp", (sp, dp), r.rest()
    if nh == 58:
        return ICMP.get(r.rest()[0], "ICMPv6 type %d" % r.rest()[0]), None, b""
    raise ValueError("next header %s" % nh)


def coap(b):
    r = Reader(b)
    h = r.u8()
    t, tkl = (h >> 4) & 3, h & 15
    code, mid = r.u8(), r.u16()
    token = r.take(tkl)
    opts, num = [], 0
    while r.i < len(b):
        x = r.u8()
        if x == 0xFF:
            break
        d, ln = x >> 4, x & 15
        d = r.u8() + 13 if d == 13 else r.u16() + 269 if d == 14 else d
        ln = r.u8() + 13 if ln == 13 else r.u16() + 269 if ln == 14 else ln
        num += d
        opts.append((num, r.take(ln)))
    return t, code, mid, token, opts, r.rest()


def uint(v):
    return int.from_bytes(v, "big") if v else 0


def code_text(code):
    if code == 0:
        return "Empty"
    if code < 32:
        return METHODS.get(code, "0.%02d" % code)
    return "%d.%02d" % (code >> 5, code & 31)


def tlvs(b):
    out, i = [], 0
    while i < len(b):
        if i + 3 > len(b):
            return None
        tag, n = int.from_bytes(b[i:i + 2], "big"), b[i + 2]
        if i + 3 + n > len(b):
            return None
        out.append((tag, b[i + 3:i + 3 + n]))
        i += 3 + n
    return out


def value_text(tag, v):
    if tag == 0x025E:
        return "<session id, %d bytes>" % len(v)
    if tag == 0x0007:
        return "<%d bytes under the device's cloud key, not shown>" % len(v)
    if len(v) > 2 and all(32 <= c < 127 for c in v):
        return '"%s"' % v.decode()
    if 1 <= len(v) <= 4:
        return "%s (%d)" % (v.hex(), uint(v))
    return v.hex()


def payload_text(path, p):
    if not p:
        return ""
    if path == "d/pair":
        return "  payload: 12 <network key, %d bytes, not shown>" % (len(p) - 2)
    t = tlvs(p)
    if t is None:
        return "  payload (%d bytes, not TLV alone): %s" % (len(p), p.hex())
    return "  payload: " + " ".join("%04x=%s" % (tag, value_text(tag, v)) for tag, v in t)


class Trace:
    def __init__(self, device, coordinator, red):
        self.device, self.coordinator, self.red = device, coordinator, red
        self.frags = {}  # (src, tag) -> bytes
        self.paths = {}  # (requester, mid) -> path
        self.raw_blocks = {}  # (requester, path) -> bytes
        self.lines = []

    def emit(self, t, s):
        self.lines.append("%s  %s" % (t, s))

    def packet(self, t, frm, to, pkt, rssi, pairing_key):
        d = pkt[0] & 0xF8
        if d == 0xC0:  # FRAG1: size, tag, then the compressed header
            tag = int.from_bytes(pkt[2:4], "big")
            self.frags[(frm, tag)] = pkt[4:]
            return
        if d == 0xE0:
            tag = int.from_bytes(pkt[2:4], "big")
            if (frm, tag) not in self.frags:
                self.emit(t, "a later fragment (tag %04x) whose first fragment was not received" % tag)
                return
            pkt = self.frags.pop((frm, tag)) + pkt[5:]
        who = "device -> coordinator" if frm == self.device else "coordinator -> device"
        kind, ports, body = iphc(pkt)
        head = "%s%s%s" % (who, " rssi %d" % rssi if rssi is not None else "",
                           " (pairing key)" if pairing_key else "")
        if kind != "udp":
            self.emit(t, "%s  %s" % (head, kind))
            return
        typ, code, mid, token, opts, raw_payload = coap(body)
        payload = raw_payload
        path = "/".join(v.decode() for n, v in opts if n == 11)
        query = "&".join(v.decode() for n, v in opts if n == 15)
        if code and code < 32:
            self.paths[(frm, mid)] = path
            req_path = path
        else:
            req_path = self.paths.get((to, mid), "")
        parts = ["%s %s mid %04x" % (TYPES[typ], code_text(code), mid)]
        if token:
            parts.append("token %s" % token.hex())
        if path:
            parts.append(path + ("?" + query if query else ""))
        elif req_path:
            parts.append("(for %s)" % req_path)
        payload = self.red.body(payload)
        block = None
        for n, v in opts:
            if n in (11, 15):
                continue
            name = OPTION_NAMES.get(n, "option %d" % n)
            if n in (23, 27):
                x = uint(v)
                block = (x >> 4, (x >> 3) & 1, 16 << (x & 7))
                parts.append("%s %d/%d/%d" % (name, *block))
            elif n == 2048:
                parts.append("Session <%d bytes>" % len(v))
            elif n in (4,):
                parts.append("%s %s" % (name, v.hex()))
            else:
                parts.append("%s %d" % (name, uint(v)))
        if ports and ports != (61616, 61616):
            parts.append("ports %d->%d" % ports)
        self.emit(t, "%s  %s" % (head, "  ".join(parts)))
        if block and code >= 64 and req_path:
            key = (to, req_path)
            # Joined before redaction, as a value may straddle two blocks.
            raw = self.raw_blocks.get(key, b"") if block[0] else b""
            self.raw_blocks[key] = raw + raw_payload
            self.lines.append("                              block %d: %d bytes" % (block[0], len(payload)))
            if not block[1]:
                body = self.red.body(self.raw_blocks.pop(key))
                self.lines.append("                              body, %d bytes: %s" % (len(body), body.hex()))
                self.lines.append("                              " + payload_text(req_path, body).strip())
            return
        text = payload_text(path or req_path, payload)
        if text:
            self.lines.append("                              " + text.strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state", required=True)
    ap.add_argument("--device", required=True, help="the device's EUI, as the host prints it")
    ap.add_argument("--serial", required=True)
    ap.add_argument("captures", nargs="+")
    a = ap.parse_args()
    props = read_properties(a.state + "/coordinator.properties")
    coordinator = props["coordinator_eui"].lower()
    device = a.device.lower()
    red = Redactor(device, coordinator, a.serial, int(props["home_id"]),
                   props.get("network_key", "").lower())

    out = []
    for path in a.captures:
        tr = Trace(bytes.fromhex(device), bytes.fromhex(coordinator), red)
        out.append("=== capture %s" % re.sub(r".*hostlink-", "hostlink-", path))
        with open(path) as f:
            for line in f:
                m = LINE.match(line.strip())
                if not m:
                    continue
                t, arrow, kind, hexs = m.groups()
                b = bytes.fromhex(hexs) if hexs and re.fullmatch(r"[0-9a-f]*", hexs) else b""
                try:
                    if kind == "RX" and len(b) > 19:
                        src = b[3:11]
                        if src == tr.device:
                            tr.packet(t, src, tr.coordinator, b[19:], b[1] - 256 if b[1] > 127 else b[1],
                                      b[0] & 1)
                    elif kind == "TX" and len(b) > 9:
                        dst = b[1:9]
                        if dst == tr.device:
                            tr.packet(t, tr.coordinator, dst, b[9:], None, False)
                        elif iphc(b[9:])[0] == "Router Advertisement":
                            tr.emit(t, "coordinator -> broadcast  Router Advertisement")
                    elif kind == "PAIR_WINDOW" and len(b) == 2:
                        secs = int.from_bytes(b, "little")
                        tr.emit(t, "pair window opened for %d s" % secs if secs else "pair window closed")
                    elif kind == "BEACON_ANSWERED" and b[:8] == tr.device:
                        tr.emit(t, "device -> coordinator  Beacon Request, answered")
                except (ValueError, IndexError, UnicodeDecodeError) as e:
                    tr.emit(t, "undecoded frame (%s)" % e)
        out += tr.lines
        out.append("")
    text = red("\n".join(out))
    leaks = red.leaks(text)
    if leaks:
        sys.exit("refusing to print: %d real identifier pieces are left in the trace" % len(leaks))
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
