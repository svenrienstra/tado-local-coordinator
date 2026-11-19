# Join probe — posing as a joiner to read a coordinator's `d/pair`

An experiment on **your own bridge and your own network**: pose as a joining
device to a coordinator and record what it sends in `d/pair`, to learn — from
primary data rather than inference — whether the commercial bridge hands the
network key as plaintext (`0x12`) or wrapped under a per-device KEK (`0x07`), and
how far its join logic proceeds for a device it does not know.

This is not part of the shipped coordinator. It lives in `probe/`, a separate
build beside `firmware/`, and is meant to be flashed only while running the
experiment. The join spec it mirrors is [`JOIN.md`](JOIN.md); the key handling it
probes is in freedo `PROTOCOL.md` ("Pairing") and summarised below.

## What we expect to find, and why

The pairing key (`"tado pairing key"`) is a fixed constant shared by every
device. It secures the join *conversation*; it does **not** decide who is
entitled to the network key. The network key travels inside `d/pair` one of two
ways (`PROTOCOL.md` "Pairing"):

- **`0x12`** — the key in plaintext. A coordinator sends this only when it holds
  no per-device KEK: ours and freedo's do, because we mint our own key.
- **`0x07`** — the key wrapped under a per-device KEK that only the device and
  the tado cloud hold.

The commercial bridge, backed by the cloud, has KEKs, so the expectation is that
it answers with `0x07` (which we cannot open) for a device it recognises, or
refuses to pair an EUI it has no record of. Posing as a joiner does not change
that: what the bridge sends depends on the EUI it sees and its KEK, not on
anything the joiner holds. So the probe is **not expected to yield a usable
key.** Its value is the primary-source answer — the `d/pair` tag on the wire, and
the step at which the bridge stops for an unknown device — which we have never
captured. (Factory reset erases a device's stored key, field `0x12`; the live key
is still present in any device that was only unplugged or pairing-reset. Reading
it out of a device you own is a chip-readout question, not this experiment.)

## Design: receive-only, MAC-only

The probe performs the **device side** of the join just far enough to make the
coordinator send `d/pair`, then records it. It does **not** adopt a key, ACK the
pair, or run `auth/token`. It transmits only a beacon request and a Router
Solicitation, both under the public pairing key.

As everywhere in this project, the radio side is MAC-only: the 6LoWPAN/CoAP
layers stay on the host. The firmware seals a datagram the host built (the RS)
and hands back the opened datagram of a `d/pair` for the host to decode with the
same tooling as [`../tools/trace`](../tools/trace).

## Sequence it performs

Mirrors [`JOIN.md`](JOIN.md) steps 2, 4 and 6, device side:

1. **Beacon Request** (broadcast, pairing key) — `joiner_beacon_request`, byte
   for byte the frame a real device sends (checked against the captured
   `beacon_request.frame`).
2. Receive the coordinator's **Enhanced Beacon** — `mac_parse_enh_beacon`
   confirms a coordinator answered and is addressed to us.
3. **Router Solicitation** (broadcast, pairing key) — the host builds the
   ICMPv6/6LoWPAN datagram, `joiner_seal` seals it from our EUI.
4. Receive the coordinator's **Router Advertisement**, then its **`d/pair`** —
   `joiner_on_rx` opens the `d/pair` under the pairing key and surfaces the
   datagram; the host decodes the CoAP and reads the key TLV tag.

No `2.04`, no key adoption, no `auth/token` (steps 7-12 of `JOIN.md` are
omitted on purpose).

## What each outcome tells us

| On air | Reading |
|---|---|
| No answer to our beacon | The bridge beacon-answers only devices the cloud expects; the EUI gates it before any key exchange |
| Beacon answered, no `d/pair` | `d/pair` is cloud-gated per device; the window (button or app) opens the radio, not the key hand-out |
| `d/pair` with tag `0x07` | Confirms the model: the key is wrapped under a per-device KEK we cannot open. First on-air capture of a real `d/pair` |
| `d/pair` with tag `0x12` | Unexpected: the bridge handed the key in plaintext. We would then hold the network key |

Any of these is a result worth recording; the first on-air `d/pair` from the
commercial bridge is the main prize regardless of the tag.

## Precautions

- **Novel EUI** for the first trial (ending in `c5 1b 00`, the required suffix):
  tests the unknown-device branch cleanly and cannot clash with a live device.
- **Leave the wall thermostat alone** — its interface-module id (`015d`) and
  boiler state are what we most want to preserve before M4 (`PLAN.md`,
  [`SENSOR.md`](SENSOR.md)). Reset a spare valve if a known EUI is wanted later.
- Minimal airtime (one beacon, one RS under the public pairing key); your own
  bridge and account.
- Optional second trial with the bridge's **internet disconnected**, to see
  whether button-pairing provisions at all offline.

## Status

- **Built and host-tested, no hardware** (`probe/`, `make test`, 19 checks):
  the device role `joiner.{c,h}`, the shared `mac_build_beacon_request` and
  `mac_parse_enh_beacon`, validated against the real captured frames and the
  same MAC primitives the coordinator and bridge use.
- **Pending (needs the CC1312):** the probe's platform layer (`probe/main.c`
  plus a flash target, like `firmware/target`) and the host driver that runs the
  sequence and decodes the tag. These, and the on-air trial itself, wait for the
  hardware.
