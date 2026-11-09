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
2. **Reuse what is proven.** The `re-lab` `minimal-rx` firmware base, the
   solved PHY, the validated CCM* (`tools/crack_pairing_key.py`), and freedo's
   `tado/` package as an executable spec + golden fixtures.
3. **Risk first.** The project lives or dies on CSL TX timing. Prove it (M0)
   before building any stack.
4. **Safety.** It controls heating. Keep a manual fallback, keep devices on their
   offline schedule, keep the CC1312 bootloader unlocked, never lock fuses.

## 3. Topology and the constraints it forces

```
[valves / wall thermostat]
   |  868 MHz CSL  (hard real-time)
[CC1312]   PHY + MAC + CSL + CCM* + local autonomy
   |  UART
[ESP32]    transparent UART <-> TCP (+TLS) bridge
   |  WiFi / LAN
[container]  Java daemon  (CoAP + 6LoWPAN + zones + schedules + HTTP API)
   |
[Home Assistant]
```

Putting the brain across a WiFi link is fine, but it forces three
non-negotiable design rules:

### 3.1 All real-time work lives on the CC1312
CSL timing — Enhanced-ACK inside the 0-1 ms post-RX gap, RAT-scheduled TX
windows, wake-up rendezvous trains (~254 frames / ~1.0 s, 4.0 ms steps) — cannot
survive a WiFi + container round-trip. The remote link may carry **only**
non-time-critical application messages (decoded frames up; CoAP/commands down),
nothing with a deadline tighter than a few hundred ms. This is why **CCM* and the
Enhanced-ACK are generated on-chip**, with keys provisioned from the host.

### 3.2 The CC1312 must keep the network alive when the brain is away
WiFi blips, container restarts and redeploys must not drop the valves. The
firmware carries a small **autonomy layer**: keep CSL alive, keep ACKing, hold
last-known setpoints/config, and let devices run their **offline schedule** while
the brain is unreachable; resync on reconnect. The brain is a **control plane**,
not a per-poll data plane.

### 3.3 Encrypt the bridge link
The control link now carries the network key and heating control over the LAN.
Wrap the ESP<->container link in **TLS** (or run it over WireGuard / a trusted
VLAN). Never ship the key as plaintext TCP.

### 3.4 Wire protocol consequence
Because the TCP tunnel can drop mid-frame and reconnect, the CC1312<->brain
protocol is **end-to-end framed**: length-prefixed, CRC'd, sequence-numbered,
with a resync marker and app-level ack/retransmit. The ESP stays dumb; the
framing is between the two smart ends. Mirror freedo's `interface_module`
*semantics* (RX-up, timed-TX-down, key provisioning, pair-window control) so
freedo stays usable as an oracle, but define our own binary wire format (it is a
Java<->C boundary, not Go).

## 4. Components

### 4.1 CC1312 firmware — the MAC engine (C)
- Base: `re-lab/firmware/cc1312-minimal-rx` (raw TI driverlib + NoRTOS RF
  driver, no Contiki netstack) — the right level for tight RF-core control.
- PHY: TX + RX on the RF core via `CMD_PROP_TX` / `CMD_PROP_RX`, RAT-timed (4 MHz
  radio timer). PHY params already solved: 868.300 MHz, 50 kbps 2-GFSK, 25 kHz
  deviation, sync `0x91D3`, 2-byte 802.15.4g PHR.
- MAC: 802.15.4-2015 framing, frame counters, dedup, Enhanced-ACK.
- CSL: wake-up rendezvous trains; timed TX into each device's wake window;
  Enhanced-ACK within the measured post-RX gap.
- Crypto: AES-CCM* (MIC-32, L=2, B0 flags 0x49, nonce = MAC[0:13], aad =
  MAC[0:16]) — port from `tools/crack_pairing_key.py`. Keys pushed from host.
- Autonomy: last-known state cache + offline-schedule keepalive (3.2).
- Host link: the framed binary protocol (3.4) over UART.

### 4.2 ESP32 bridge — the wire (C / ESPHome)
- Transparent UART<->TCP at 460800 baud, auto-reconnect, **TLS**.
- Zero protocol logic; it moves bytes. Options: ESPHome stream-server, a small
  ESP-IDF sketch, or (more robust alternative) a tiny Linux box at the radio spot
  running `ser2net` — which could even host the container and remove the lossy
  link entirely.

### 4.3 Java daemon — the brain (containerised)
- CoAP (RFC 7252): **Eclipse Californium** (CON/ACK, block-wise).
- 6LoWPAN frag/reasm, MAC state, CCM* (for non-time-critical paths / validation),
  the tado resource model: `d/pair`, `auth/token`, `time`, `d/<serial>/config`,
  zone configs, resources.
- Coordinator logic: pair window, **network-key generation**, device DB,
  zones/schedules, persistence, frame-counter resume.
- Serial-over-TCP client (jSerialComm for local USB bench; TCP for the ESP
  bridge) implementing the framed protocol with reconnect tolerance.
- HTTP API for Home Assistant + a UI (later).
- freedo's `tado/` replayed as golden fixtures to byte-match our parsing.

## 5. Milestones (each is a go/no-go gate)

### M0 — De-risk TX + CSL timing  *(make-or-break; firmware-only; start first)*
1. CC1312 transmits one valid frame a second receiver MIC-verifies under a known
   key.
2. RAT-scheduled timed TX lands in a target window at < +/-100 us jitter.
3. Generate a valid Enhanced-ACK within the 0-1 ms gap after an RX.

**Gate:** if host-driven CC1312 TX cannot meet CSL timing, fall back to fully
on-chip timing, or reconsider an ESP32+CC1101 bridge (freedo's path). Everything
below assumes M0 passes. Language/host decisions do not block M0.

### M1 — Clean-room protocol harness  *(no radio)*
Replay freedo `testdata` + the `re-lab` captures through the Java
decoder / CoAP / CCM* and **byte-match freedo's parsing**. Builds the brain
against a known-good oracle.

### M1.5 — Transport bridge
CC1312 <-> ESP32 <-> TCP(+TLS) <-> Java daemon with the framed,
reconnect-tolerant protocol. Prove a decoded frame reaches the brain and a
command returns, **across a forced WiFi drop**.

### M2 — First join
With one **factory-reset valve**: answer its Beacon Requests + Router
Solicitation, complete `d/pair` with our own tag-`0x12` network key, device
reports temperature. Includes the CC1312 autonomy layer (3.2).

**Gate:** a valve joins *our* coordinator and sends a decodable report. This is
the payoff — full local control, zero key extraction.

### M3 — Control writes
Setpoint, boost, home/away to a valve; verify via its reports.

### M4 — Scale + robustness
Multi-device, wall thermostat + hot water, schedules, persistence, frame-counter
resume across restart, brain-outage survival.

### M5 — Integration  *(optional / later)*
HTTP API + Home Assistant. Shortcut: if the API mirrors freedo, point freedo's
existing HA integration at it.

## 6. Testing strategy
- **Golden fixtures** from freedo `testdata` + our captures; CCM* test vectors.
- **Unit tests** for CoAP / 6LoWPAN / MAC / CCM* in Java against those fixtures.
- **Firmware timing** measured via RAT logs / logic analyser, not assumed.
- **HIL**: one spare valve on the bench for M2+.

## 7. Legal & safety
- Interoperates with devices **you own**; tado trademark used descriptively only.
- **Confirm freedo's licence** (`LICENSE` + `NOTICE` in that repo) before copying
  any code; otherwise treat `tado/` as a clean-room spec and fixtures only.
- A device joined here **leaves the tado cloud/app** (single coordinator —
  inherent).
- Keep a manual heating fallback and the offline schedule active throughout;
  keep the CC1312 bootloader/debug unlocked; never enable fuse/read-out locks.

## 8. Open decisions (to confirm before/while scaffolding code)
- Bridge: ESP32 (ESPHome vs ESP-IDF) vs a tiny Linux `ser2net` box.
- Java build/runtime: Gradle vs Maven; JDK version; base container image.
- Persistence: embedded (SQLite/H2) vs flat files for the device DB + schedules.
- Whether the brain also keeps a thin local cache to further decouple from the
  CC1312 autonomy layer.

## 9. Heritage / cross-references
- `re-lab` — reverse-engineering lab: solved PHY, decoders,
  `tools/crack_pairing_key.py` (CCM* + pairing-key recovery), captures.
- `CappyT/freedo` — ESP32+CC1101 reimplementation; behavioural oracle. Key facts
  used: pairing key `"tado pairing key"` (TLV tag `0x12` plaintext vs `0x07`
  cloud-KEK-wrapped), CCM* parameters, CSL/Enhanced-ACK timing, resource model.
