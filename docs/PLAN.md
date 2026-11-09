# Build plan — clean-room tado coordinator on CC1312

Status: **planning / not started**. This document is the agreed design and
sequencing. It supersedes ad-hoc notes; update it as decisions firm up.

---

## 1. Goal

Replace the tado internet bridge for the radiator valves and wall thermostats of
one home, under fully local control, built from scratch:

- **CC1312R1** as the radio / MAC / CSL real-time engine.
- An **ESP32** as a transparent UART<->WiFi bridge (dumb pipe only).
- A **Java daemon in a container** as the control brain, exposing an HTTP API to
  Home Assistant.

No tado cloud, no HomeKit, no device key extraction (we mint the network key as
the coordinator — see the README).

## 2. Guiding principles

1. **Two tiers, timing on-chip.** The CC1312 owns everything with a deadline;
   the Java brain owns everything without one. freedo split this way because its
   CC1101 is dumb; we split this way on purpose so the brain can live in a
   remote container.
2. **Reuse what is proven.** freedo's `tado/` package and `docs/PROTOCOL.md` as
   an executable spec + golden fixtures, and our own captures of real traffic.
3. **Risk first.** The project lives or dies on one question: does a device
   accept a network key *we* mint? Prove that (M-1) before building any stack,
   then confirm CSL timing (M0).
4. **Safety.** It controls heating. Keep a manual fallback, keep devices on their
   offline behaviour, keep the CC1312 bootloader unlocked, never lock fuses.

## 3. Topology and the constraints it forces

```
[valves / wall thermostat]
   |  868 MHz CSL  (hard real-time)
[CC1312]   PHY + MAC + CSL + CCM* + local autonomy
   |  UART  (link-encrypted frames)
[ESP32]    transparent UART <-> TCP bridge
   |  WiFi / LAN
[container]  Java daemon  (CoAP + 6LoWPAN + zones + schedules + HTTP API)
   |
[Home Assistant]
```

Putting the brain across a WiFi link is fine, but it forces these
non-negotiable design rules:

### 3.1 All real-time work lives on the CC1312
CSL timing cannot survive a WiFi + container round-trip. Reference timings
(freedo's on-air measurements; re-measure ourselves in M0):

- Enhanced-ACK starts **0-1 ms after the Data frame ends** (the 29-byte ACK
  itself ends 6-7 ms after the Data frame; freedo's "5.0-8.0 ms after it" is
  measured end to end and includes the ACK's own airtime).
- Data frame 9-24 ms after the last wake-up frame (median 12 ms).
- Wake-up trains, 4.0 ms frame step: rendezvous 248->0 in 21 frames / 79 ms,
  or 3125->0 in 254 frames / 1.003 s. Only the coordinator sends trains; a
  broadcast goes behind the full 1.003 s train.
- Pairing beacon answer: no wake-up train, 2.4-7.4 ms after the request.
- Retransmits: devices after 1.5-3.6 s, the coordinator after 2-6 s.

Enhanced-ACKs are **not secured** (a software FCS only, no MIC), so the ACK path
needs no crypto. The remote link may carry **only** non-time-critical
application messages (decoded frames up; CoAP/commands down), nothing with a
deadline tighter than a few hundred ms.

### 3.2 The CC1312 must keep the network alive when the brain is away
WiFi blips, container restarts and redeploys must not drop the valves. The
firmware carries a small **autonomy layer**; the brain is a **control plane**,
not a per-poll data plane.

Known (freedo `PROTOCOL.md`, "Router advertisement"): a device that receives no
ICMPv6 Router Advertisement for about **20 minutes stops reporting** and floods
Router Solicitations; an advertisement brings it back within ~4 minutes. So the
autonomy layer must at minimum:

- broadcast a Router Advertisement every 200-600 s (behind a 254-frame wake-up
  train) and answer Router Solicitations (not within 3 s of the last one). The
  RA payload is pre-built by the brain and cached on the CC1312, which only
  re-secures it with a fresh seq/counter;
- keep Enhanced-ACKing device uplinks.

Still open, to answer from freedo + observation before M4:

- What does a valve do while the brain is away but RAs continue: keep its
  schedule, hold its setpoint? Its CoAP uplinks (CON) go unanswered — how long
  until it gives up or rejoins?
- `time` must be answered within 5 s of the request (the valve discards later
  answers), so it cannot be served while the brain is away; does a missing
  `time` answer matter in steady state?
- Downlink queue for sleeping devices: depth per device, entry expiry, and what
  happens to queued frames when the brain reconnects (resync rules).

Whatever the answers, the CC1312 must survive **its own** power loss while the
brain is away: the network key, the outgoing frame counter (3.5) and per-device
state live in **flash**, not just RAM.

### 3.3 Encrypt the bridge link end-to-end
The control link carries the network key and heating control over the LAN.
Rather than terminating TLS on the ESP32 (no TLS in ESPHome's stream server;
certificate handling on the ESP is real work), the framed protocol (3.4) is
itself **AES-CCM-encrypted between the CC1312 and the Java daemon** with a
pre-shared link key (the CC1312 has a hardware AES engine). The ESP stays a
truly dumb pipe, and the network key is never plaintext even on the UART.
WireGuard / a trusted VLAN remain optional extra layers.

### 3.4 Wire protocol consequence
Because the TCP tunnel can drop mid-frame and reconnect, the CC1312<->brain
protocol is **end-to-end framed**: length-prefixed, CRC'd, sequence-numbered,
encrypted (3.3), with a resync marker and app-level ack/retransmit. The framing
is between the two smart ends. Mirror freedo's `interface_module` *semantics*
(RX-up, timed-TX-down, key provisioning, pair-window control) so freedo stays
usable as an oracle, but define our own binary wire format (it is a Java<->C
boundary, not Go).

### 3.5 One owner for MAC seq and frame counter
The CCM* nonce is `frame[0:13]` = FCF + MAC seq + destination EUI + 2 source
bytes; the application frame counter sits in the encrypted payload and is
**not** in the nonce. Devices accept lower counters (no anti-replay). So
nonce uniqueness rests on the 1-byte MAC seq, and retransmissions reuse the seq
with a new counter — nonce reuse is a property of the protocol, not something
we can engineer away. What we can keep clean is bookkeeping: the commercial
coordinator increments the counter once per frame with no reuse, and broadcasts
take "last unicast counter + 1".

Rule: **the CC1312 is the sole allocator of MAC seq and frame counter and the
sole encryptor** of outgoing link frames (it must originate beacons, RAs and
autonomy traffic anyway). It persists the counter to flash in reserved blocks
(write a floor N ahead, skip to it on boot — freedo uses 4096). The Java daemon
builds plaintext payloads and uses CCM* for **decryption / validation only**.

### 3.6 Radio duty cycle
868.0-868.6 MHz is an ETSI EN 300 220 sub-band with a 1 % duty-cycle limit
without LBT+AFA (confirm the exact band rules), and freedo found LBT broke
delivery on air. 1 % = 36 s/hour of TX; a full 1.003 s wake-up train is ~1 s,
so long trains are a scarce budget shared across all devices. The firmware
enforces an **airtime limiter** and the brain plans traffic against it (prefer
short trains for synchronised devices, batch writes).

## 4. Components

### 4.1 CC1312 firmware — the MAC engine (C)
- Base: raw TI driverlib + NoRTOS RF driver (no Contiki/TI-15.4 netstack) — the
  right level for tight RF-core control. TX and RX both written here.
- PHY: TX + RX on the RF core via `CMD_PROP_TX` / `CMD_PROP_RX`, RAT-timed (4 MHz
  radio timer). PHY params, from freedo's working CC1101 register set
  (`cc1101/cc1101.go`): ~868.3 MHz (freedo: channel 26 = 868.324 MHz),
  50 kbps 2-GFSK, 25 kHz deviation, sync word `0xD391` on air, **1-byte length
  field**, no whitening, CC1101-style hardware CRC-16. (Earlier notes said
  "sync `0x91D3` + 2-byte 802.15.4g PHR"; that was the second sync byte read as
  a PHR byte.)
- MAC: 802.15.4-2015 framing, seq + frame counters (3.5), dedup, Enhanced-ACK
  (unsecured, 29 bytes with CSL IE and software FCS, 0-1 ms after RX ends),
  Enhanced Beacon answer to Beacon Requests (2.4-7.4 ms, no train).
- CSL: wake-up rendezvous trains; timed TX into each device's wake window.
- Crypto: AES-CCM* (MIC-32, L=2, B0 flags 0x49, nonce = MAC[0:13], aad =
  MAC[0:16]), on the hardware AES engine. Verify against freedo's `tado/ccm.go`
  and pairing-key-secured frames from our own captures. Keys pushed from host,
  persisted in flash.
- Autonomy: state cache + keepalive (3.2); airtime limiter (3.6).
- Host link: the framed, encrypted protocol (3.3, 3.4) over UART.

### 4.2 ESP32 bridge — the wire (C / ESPHome)
- Transparent UART<->TCP at 460800 baud, auto-reconnect. No TLS needed — the
  payload is already encrypted end-to-end (3.3).
- Zero protocol logic; it moves bytes. Options: ESPHome stream-server, a small
  ESP-IDF sketch, or (more robust alternative) a tiny Linux box at the radio spot
  running `ser2net` — which could even host the container and remove the lossy
  link entirely.
- **Remote CC1312 flashing:** wire ESP GPIOs to the CC1312 `RESET_N` and the ROM
  bootloader backdoor pin, so the CC1312 can be reflashed over the network via
  its UART serial bootloader (cc2538-bsl protocol). Plus ESP OTA for the bridge
  itself.

### 4.3 Java daemon — the brain (containerised)
- CoAP (RFC 7252): **Eclipse Californium** (CON/ACK, block-wise).
- 6LoWPAN frag/reasm, MAC state mirror, CCM* **decrypt/validate only** (3.5),
  the tado resource model: `d/pair`, `auth/token`, `time`, `d/<serial>/config`,
  zone configs, resources.
- Coordinator logic: pair window, **network-key generation**, device DB,
  zones/schedules, persistence, airtime-aware scheduling (3.6).
- Serial client (jSerialComm for a USB-attached CC1312; TCP for the ESP bridge)
  implementing the framed protocol with reconnect tolerance.
- HTTP API for Home Assistant + a UI (later).
- freedo's `tado/` replayed as golden fixtures (see M1 on oracle mismatches).

## 5. Milestones (each is a go/no-go gate)

M1 needs no radio and runs **in parallel** with M-1/M0 from day one.

### M-1 — Prove the thesis: a valve joins with *our* key  *(start first)*
Replicate freedo's on-air join on **one valve**, following the frame-by-frame
spec in [`JOIN.md`](JOIN.md) steps 1-7: button long-press (~2 min
commissioning window), answer its Enhanced Beacon Request (2.4-7.4 ms, no
train), answer its Router Solicitation with the doubled Router Advertisement,
send `d/pair` with our own tag-`0x12` network key behind a full 254-frame
wake-up train, and see the valve answer `2.04` under the new key. USB-attached
CC1312; this is a thesis test, not a stack.

Firmware needed: RX + TX on the PHY, CCM*, the on-chip beacon answer, and
full-length wake-up trains. Not needed: the 0-1 ms Enhanced-ACK (a missed ACK
only makes the valve retransmit its `2.04`) and anchored short trains.

Evidence so far: freedo reports this working on air for one valve (it can only
send `0x12`, having no cloud KEK). The genuine bridge has only ever been seen
sending `0x07`. Wall-thermostat pairing is **not** verified anywhere — repeat
M-1 for a wall thermostat before M4 depends on it.

**Gate:** if a valve will not adopt a `0x12` key, the project's premise fails;
stop and reassess.

### M0 — Tight CSL timing  *(firmware-only)*
The timing M-1 could skip:
1. Generate a valid (unsecured) Enhanced-ACK starting 0-1 ms after an RX ends,
   with a correct CSL IE; measure the real gap on our own captures.
2. Track each device's CSL phase from its Enhanced-ACKs and land a 21-frame
   (79 ms) short train + Data frame in its window, at < +/-100 us jitter.

**Gate:** all timing is on-chip by design (3.1). freedo's bridge firmware meets
these windows (ESP32 + CC1101), so this is expected to pass; if it does not,
reconsider an ESP32+CC1101 radio (freedo's path).

### M1 — Clean-room protocol harness  *(no radio; parallel)*
Replay freedo `testdata` + our own captures through the Java decoder / CoAP /
CCM* and match freedo's parsing. freedo is an oracle, not ground truth: a
mismatch is investigated against raw captures, not auto-fixed to match.

### M2 — First join, end to end  *(CC1312 over USB)*
With one button-reset valve and the CC1312 on USB to the daemon (jSerialComm),
using the framed protocol: the full [`JOIN.md`](JOIN.md) sequence, steps 1-12 —
`d/pair` with a daemon-generated network key, then `auth/token`, RA, `time`,
`d/<serial>/config` — until the device reports temperature. No ESP/WiFi and no
autonomy layer yet.

**Gate:** a valve joins *our* coordinator and sends a decodable report through
the Java stack.

### M3 — Control writes
Setpoint, boost, home/away to a valve; verify via its reports.

### M3.5 — Transport bridge
CC1312 <-> ESP32 <-> TCP <-> Java daemon with the framed, encrypted,
reconnect-tolerant protocol. Prove a decoded frame reaches the brain and a
command returns, **across a forced WiFi drop**. Remote CC1312 reflash via the
ESP works.

### M4 — Scale + robustness
Multi-device, wall thermostat (after its M-1 check) + hot water, schedules,
persistence, frame-counter persistence across CC1312 reboot (3.5), autonomy
layer and brain-outage survival (3.2), airtime budget under realistic load
(3.6).

### M5 — Integration  *(optional / later)*
HTTP API + Home Assistant. Shortcut: if the API mirrors freedo, point freedo's
existing HA integration at it.

## 6. Testing strategy
- **Golden fixtures** from freedo `testdata` + our captures; CCM* test vectors.
- **Unit tests** for CoAP / 6LoWPAN / MAC / CCM* in Java against those fixtures.
- **Fake CC1312** link endpoint for daemon tests (reconnects, drops, resync).
- **Firmware timing** measured via RAT logs / logic analyser, not assumed.
- **HIL**: one spare valve on the bench from M-1 onward.

## 7. Legal & safety
- Interoperates with devices **you own**; tado trademark used descriptively only.
- freedo is **Apache-2.0** (`LICENSE` + `NOTICE`, "Copyright 2026 The freedo
  contributors"). Code may be reused with attribution; carry its NOTICE if we do.
  The footprint libraries under its `hardware/radio/lib` keep their own terms.
- A device joined here **leaves the tado cloud/app** (single coordinator —
  inherent). Joining requires a **physical button long-press** on the device
  (no remote commissioning). To go back: long-press reset, then pair again from
  the tado app.
- Keep a manual heating fallback and device offline behaviour active
  throughout; keep the CC1312 bootloader/debug unlocked; never enable
  fuse/read-out locks.

## 8. Defaults (change only with a reason)
- Bridge: ESPHome stream-server first; a tiny Linux `ser2net` box if the WiFi
  link proves lossy.
- Java: Maven (with wrapper), Java 25, a distroless/temurin JRE base image.
- Persistence: SQLite for the device DB + schedules.
- The brain keeps no extra cache beyond what the CC1312 autonomy layer (3.2)
  requires; revisit after M4.

## 9. Heritage / cross-references
- `CappyT/freedo` (Apache-2.0) — ESP32+CC1101 reimplementation; behavioural
  oracle. Key facts used: pairing key `"tado pairing key"` (TLV tag `0x12`
  plaintext vs `0x07` cloud-KEK-wrapped), CCM* parameters, CSL / Enhanced-ACK
  timing, commissioning via button, resource model.
