# Build plan — clean-room tado coordinator on CC1312 + ESP32

Status: **M0 works on air, M1 done, M2 built and tested on the Mac and
waiting for the ESP32-S3 board**. This document is the agreed design and
sequencing. It supersedes ad-hoc notes; update it as decisions firm
up.

---

## 1. Goal

Replace the tado internet bridge for the radiator valves and wall thermostats of
one home, under fully local control, built from scratch, as one self-contained
device:

- **CC1312R1** as the radio / MAC / CSL real-time engine.
- An **ESP32-S3 gateway** running the tado protocol and the coordinator logic. It
  publishes device state (temperature, setpoint, battery, …) as **MQTT** events
  and takes commands (setpoint, pair window, …) over MQTT.
- **Home Assistant** picks the devices up through MQTT discovery. No container or
  always-on server is needed.

The Java program in `host/` is the **bench tool** for M-1 (CC1312 on USB to a
Mac). It is not deployed.

No tado cloud, no HomeKit, no device key extraction (we mint the network key as
the coordinator — see the README).

## 2. Guiding principles

1. **Two tiers, timing on-chip.** The CC1312 owns everything with a deadline;
   the ESP32 owns everything without one.
2. **Keep the CC1312 small and stable.** Its firmware is the hard part to
   update: remote reflash may not be possible on this board
   ([`WIRING.md`](WIRING.md)). The ESP32 updates over WiFi (OTA), so logic that
   will change (the tado resource model, MQTT, device handling) lives there.
3. **Reuse what is proven.** freedo's `tado/` package and `docs/PROTOCOL.md` as
   an executable spec + golden fixtures, and our own captures of real traffic.
4. **Risk first.** The project lives or dies on one question: does a device
   accept a network key *we* mint? Prove that (M-1) before building any stack,
   then confirm CSL timing (M0).
5. **Safety.** It controls heating. Keep a manual fallback, keep devices on their
   offline behaviour, keep the CC1312 bootloader unlocked, never lock fuses.

## 3. Topology and the constraints it forces

```
[valves / wall thermostat]
   |  868 MHz CSL  (hard real-time)
[CC1312]   PHY + MAC + CSL + CCM* sealing + keepalive autonomy
   |  UART 460800, host link (framed, CRC'd; docs/HOSTLINK.md)
[ESP32]    gateway: 6LoWPAN + CoAP + tado resources + device DB + MQTT + SNTP + OTA
   |  WiFi / LAN, MQTT (no TLS)
[MQTT broker]  e.g. Mosquitto (Home Assistant add-on)
   |
[Home Assistant]
```

The CC1312 and the ESP32 sit next to each other, wired by UART and powered
together. The only network hop is MQTT.

### 3.1 All real-time work lives on the CC1312
CSL timing cannot survive a UART round-trip through the ESP. Reference timings,
measured on our CC1312 against the commercial bridge (2026-11-17, 154 trains
and 286 ACKs; `firmware/README.md` "On air"):

- Enhanced-ACK starts **0.35-0.45 ms after the Data frame ends** (ours: 0.70 ms;
  the frame reaches our main loop 0.49 ms after its end and the ACK is posted
  by about 0.66 ms).
- Data frame **0.52 ms (broadcast) or a median 0.82 ms (unicast) after the last
  wake-up frame ends**. freedo's "9 to 24 ms after the last wake-up frame
  (median 12 ms)" is wrong; a train built on it wakes a device 16-24 ms early
  and the Data frame misses it.
- Wake-up trains, **3968 us frame step** (12.4 rendezvous units): rendezvous
  248->0 in 21 frames centred on the device's sample, or 3125->0 in 254 frames
  / 1.05 s. Only the coordinator sends trains; a broadcast goes behind the full
  train.
- A device's sample: Enhanced-ACK end minus its CSL phase x 320 us, then every
  1 s; stable within 0.3 ms over minutes.
- Pairing beacon answer: no wake-up train, 2.4-7.4 ms after the request.
- Retransmits: devices after 1.5-3.6 s, the coordinator after 2-6 s.

Enhanced-ACKs are **not secured** (a software FCS only, no MIC), so the ACK path
needs no crypto. The host link carries **only** non-time-critical application
messages (decoded frames up; CoAP/commands down).

### 3.2 Failure domains
Two things can go away while the valves must stay joined:

- **WiFi or the MQTT broker.** The ESP keeps running the network: it answers
  `time`, `auth/token` and device uplinks, and keeps its device state. Home
  Assistant loses control and readings until MQTT returns. Commands sent while
  disconnected are lost, not replayed (3.7).
- **The ESP itself** (OTA reboot, crash, watchdog reset: seconds). The CC1312
  keeps Enhanced-ACKing uplinks and broadcasting Router Advertisements, so the
  valves do not notice. The RA payload is pre-built by the ESP and cached on
  the CC1312, which only re-secures it with a fresh seq/counter. Zone traffic
  between devices (3.8) pauses, because the ESP relays it; the wall thermostat
  keeps the last heat demand it got for up to its 30-minute Max-Age, so a
  reboot of seconds goes unnoticed.

Known (freedo `PROTOCOL.md`, "Router advertisement"): a device that receives no
ICMPv6 Router Advertisement for about **20 minutes stops reporting** and floods
Router Solicitations; an advertisement brings it back within ~4 minutes. The
CC1312 broadcasts an RA every 200-600 s (behind a 254-frame wake-up train) and
answers Router Solicitations (not within 3 s of the last one).

Still open, to answer from freedo + observation before M4:

- `time` must be answered within 5 s of the request (the valve discards later
  answers), so it goes unanswered while the ESP reboots; does a missing answer
  matter in steady state?
- Downlink queue for sleeping devices: depth per device, entry expiry.

Persistence: both chips lose power together, so state lives on the ESP (NVS
flash): the network key, the frame-counter floor (3.5) and the device table. The
ESP sends them to the CC1312 with `CONFIG` after every CC1312 boot; the CC1312
keeps them in RAM only.

### 3.3 No transport encryption on the LAN
The UART is a wire on the bench, inside one enclosure, so the host link is not
encrypted. MQTT runs without TLS, with broker username and password.

Accepted risk: anyone who can sniff the LAN sees heating state and the MQTT
credentials, and with those could change setpoints. The **network key never
leaves the ESP** and is never published over MQTT. If the LAN is not trusted
later, ESP-IDF's MQTT client supports TLS; switching it on changes no topics.

### 3.4 Host link
The CC1312<->ESP protocol stays the framed v0 protocol of
[`HOSTLINK.md`](HOSTLINK.md): length-prefixed, CRC'd, sequence-numbered, with a
resync marker and request/reply retransmit. Over a direct UART this covers
resets of either side and line noise. Mirror freedo's `interface_module`
*semantics* (RX-up, timed-TX-down, key provisioning, pair-window control) so
freedo stays usable as an oracle.

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
autonomy traffic anyway). The ESP reserves the counter in blocks: it stores a
floor N ahead in NVS and gives the CC1312 the old floor in `CONFIG` (freedo uses
4096). The ESP builds plaintext payloads and uses CCM* for **decryption /
validation only**.

### 3.6 Radio duty cycle
868.0-868.6 MHz is an ETSI EN 300 220 sub-band with a 1 % duty-cycle limit
without LBT+AFA (confirm the exact band rules), and freedo found LBT broke
delivery on air. 1 % = 36 s/hour of TX; a full 1.003 s wake-up train is ~1 s,
so long trains are a scarce budget shared across all devices. The firmware
enforces an **airtime limiter** and the ESP plans traffic against it (prefer
short trains for synchronised devices, batch writes).

### 3.7 MQTT interface
A first sketch; fixed in M2/M3. Home Assistant controls **rooms (zones, 3.8)**,
not single valves.

As built in M2 and M3 (`gateway/app/ha.c`): sensors, each published
(discovery, retained) once it has a value: per device temperature, humidity,
battery (V), signal and last seen; per zone temperature, setpoint and heating
demand, named after the room. Each room is also a climate entity (heat only,
5-25 C in 0.1 steps): its target, current temperature and action (heating or
idle) come from `tado/zone/<n>/state`, so what the device reported; a new
target goes to `tado/zone/<n>/setpoint/set`. `tado/gateway/pair/set` opens a
pair window, `tado/home/set` takes `home` or `away` (an Away switch on the
gateway's device), and `tado/zone/<n>/preset/set` takes `boost` or `none`
(the climate entity's preset). Empty and retained commands are ignored: an
old command again at every reconnect, or its clearing.

- **Discovery:** for each zone, a retained config message on
  `homeassistant/climate/tado_zone_<n>/config`: one climate entity per room.
  Each device also gets sensors (battery, signal, its own temperature, valve
  opening). Devices and zones appear at runtime, so discovery messages are
  published and removed as they come and go.
- **Zone state (retained):** `tado/zone/<n>/state`, JSON, e.g.
  `{"temperature": 20.4, "setpoint": 21.0, "demand": 35, "target": 21.5,
  "confirmed": false}`. The temperature is the zone's measure (3.8), and
  temperature, setpoint and demand are what the devices reported, never what
  was asked for. `target` is the gateway's own word (the room's setpoint, away
  temperature or boost), which the climate entity shows so its slider stays
  where it was set; `confirmed` says whether the device took it (no write
  pending and its report agrees). The climate entity carries `confirmed` and
  `reported_setpoint` as attributes (decided 2026-11-18). A room whose device has not
  confirmed for a set time (setup page, default 30 min, 0: never) gets
  `"stale": true` and its climate entity goes unavailable; a missed write
  normally heals within about 20 minutes, when the device reads its zone
  state again and gets the gateway's.
- **Device state (retained):** `tado/device/<serial>/state`, e.g.
  `{"temperature": 20.9, "battery": "ok", "rssi": -71, "last_seen": "…"}`.
- **Commands (not retained):** `tado/zone/<n>/setpoint/set` with a number in °C.
  The gateway writes the zone's writer (3.8). Devices sleep between CSL
  wake-ups, so a write is queued, sent in the device's next window, and
  confirmed by the next report. A command that cannot be delivered within an
  expiry time is dropped and logged.
- **Gateway:** `tado/gateway/status` online/offline (MQTT last will),
  `tado/gateway/pair/set` opens a pair window, `tado/gateway/log` carries
  warnings and the CC1312 `LOG` lines.

Schedules and away mode stay in Home Assistant for now (automations writing
zone setpoints). Which tado features the gateway exposes beyond that is decided
after M3.

### 3.8 Zones (rooms) and the boiler
The devices regulate the rooms and the boiler themselves; the coordinator
**configures** that and **relays** the traffic between them. Everything here is
from freedo's `docs/PROTOCOL.md` (firmware reading plus one plant), not yet
from our own captures.

- **A zone is a document the coordinator serves**, `h/<home>/z/<n>/config`
  (about 300 bytes, sent block-wise): its kind (heating or hot water), its
  members, its **owner** (the measuring endpoint), the circuit it belongs to,
  and the controller gains. Each device learns its zone and role from tag
  `015e` in its own `d/config`.
- **The owner measures and decides.** In a room with a wall thermostat, the
  thermostat is the natural owner: its temperature is the room's temperature.
  In a room with only valves, one valve is elected. The owner runs a PI(D)
  controller on its temperature against the zone setpoint and publishes a heat
  demand (0-100 %). The other valves in the zone follow the owner's measure and
  demand, so two valves in one room open together on the thermostat's reading.
- **The boiler is the wall thermostat's job.** It is the heat source of the
  circuit: it observes each zone owner's demand, takes the maximum over the
  zones of its circuit, and drives the boiler. The coordinator never writes a
  boiler setpoint; it only binds each heating zone to the thermostat's circuit
  in the zone document. (freedo can also cap the flow temperature; later, if
  wanted.) The thermostat also carries the hot water zone.
- **Devices do not talk directly.** Every device sleeps between CSL windows, so
  zone traffic (measures, demand, setpoint echoes) goes through the coordinator,
  which forwards each message byte for byte into the recipient's next window.
- **Setpoints go to one device per zone**, the zone's writer: the owner where
  there is one. The others take the new setpoint from its echo.
- **Gateway state:** a room table in NVS (zone number, name, kind, members,
  owner) and the joined devices. As in the tado app, a device is paired
  first and then put in a room on the setup page (3.9).
- **The owner is the room's controller**, chosen by default by what measures
  the room best: the wall thermostat if the room has one, else a wireless
  temperature sensor (`SENSOR.md`: it runs a zone as owner), else a valve.
  The setup page can pick another one (3.9).
- **Later option:** the coordinator as owner of a zone, regulating on a
  temperature sensor from Home Assistant. freedo does this; it adds a
  controller on the gateway.
- **Wanted, to test (M5): rooms that heat only while the boiler already
  runs.** No command sets a valve's opening directly; a valve opens from its
  zone's demand. Two ways to keep a room's demand away from the boiler, both
  untested on any device:
  1. Leave the zone out of the boiler circuit (no tag `6060` in its zone
     document). Its valves still regulate to their setpoint, but freedo reads
     the thermostat firmware as sending such a zone's demand to a circuit
     nobody serves.
  2. The coordinator as zone owner (above), sending the valves one demand and
     the thermostat 0 %.

  The gateway sees whether the boiler runs from the thermostat's reports
  (`c/1/act`, `hvac/mon`), so it can also open such rooms only then.

Consequence: the wall thermostat matters for every room, because the boiler
only fires on its demand. Its pairing is not verified anywhere yet (M-1 check
before M4).

### 3.9 Setup page
Rooms are configured on a small web page that the ESP serves (ESP-IDF HTTP
server, one static page plus a JSON API, password-protected, no TLS — as 3.3).
Home Assistant stays the place for daily use (temperatures, setpoints,
schedules); it cannot offer a decent form for this.

- **Pair first, then assign a room**, as in the tado app. Add device opens a
  pair window; the device that joins is listed under **No room**. Until it
  has a room its `GET d/<serial>/config` goes unanswered, and it asks again
  about once a minute: the sensor did exactly this after its first join
  (`SENSOR.md`), and took the config the first time one was served. Putting
  it in a room makes the gateway answer, so the device picks up its room
  within a minute and nothing is written to it.
- Rooms: create, rename, delete (its devices go back to No room); move
  devices between rooms; the room's controller (its owner, 3.8): the
  thermostat or sensor by default, any member on request; the zone options
  of 3.8.
- Devices: list with status, battery and last seen, and whether one is
  waiting for a room.
- Moving a device that already holds a room's config takes a config write or
  `POST d/reboot` to reach it, and rewriting a valve's `d/config` makes it
  home its motor (a full stroke, battery): such changes are applied
  explicitly (edit, then apply), not per click. Not built yet (M4); until
  then a move reaches a device at its next config read.

**Built first (M2, 2026-11-18):** Add device, rooms (create, rename,
delete), putting a device in a room or back to No room, and the device list.
One device per room until M4 brings members, so that device is the
controller; choosing one comes with members.

**Fallback if the ESP runs out of room** (flash, heap, or CPU next to the
protocol work; measured at M4): move the setup page off the ESP, either into a
custom Home Assistant integration (freedo's route) or a small container on the
LAN that talks to the gateway's JSON API. The gateway keeps the room table
either way.

## 4. Components

### 4.1 CC1312 firmware — the MAC engine (C)
- Base: raw TI driverlib + NoRTOS RF driver (no Contiki/TI-15.4 netstack) — the
  right level for tight RF-core control. TX and RX both written here.
- PHY: TX + RX on the RF core via `CMD_PROP_TX` / `CMD_PROP_RX`, RAT-timed (4 MHz
  radio timer). PHY params, from freedo's working CC1101 register set
  (`cc1101/cc1101.go`): ~868.3 MHz (freedo: channel 26 = 868.324 MHz),
  50 kbps 2-GFSK, 25 kHz deviation, sync word `0xD391` on air, **1-byte length
  field**, no whitening, CC1101-style hardware CRC-16.
- MAC: 802.15.4-2015 framing, seq + frame counters (3.5), dedup, Enhanced-ACK
  (unsecured, 29 bytes with CSL IE and software FCS, 0-1 ms after RX ends),
  Enhanced Beacon answer to Beacon Requests (2.4-7.4 ms, no train).
- CSL: wake-up rendezvous trains; timed TX into each device's wake window.
- Crypto: AES-CCM* (MIC-32, L=2, B0 flags 0x49, nonce = MAC[0:13], aad =
  MAC[0:16]), on the hardware AES engine. Keys come from the ESP in `CONFIG`
  and are held in RAM.
- Autonomy: Enhanced-ACKs and the RA keepalive while the ESP is away (3.2);
  airtime limiter (3.6).
- Host link: the framed protocol (3.4) over UART.

### 4.2 ESP32 gateway — the brain (C, ESP-IDF)
- Host link client over UART1 at 460800 baud; resets the CC1312 through its
  `RESET_N` (wiring: [`WIRING.md`](WIRING.md)).
- 6LoWPAN, CoAP (CON/ACK, block-wise where the devices use it), CCM*
  **decrypt/validate only** (3.5). The portable C core of `firmware/src`
  (`crc`, `ccm`, `mac`) is shared, not rewritten.
- The tado resource model: `d/pair`, `auth/token`, `time`,
  `d/<serial>/config`, zone documents and zone reads, device reports and
  setpoint writes, and relaying zone traffic between devices (3.8).
- Coordinator logic: pair window, **network-key generation** (hardware RNG),
  device table and counter floor in NVS, per-device downlink queue,
  airtime-aware scheduling (3.6).
- SNTP for wall-clock time (`time` answers).
- MQTT client (ESP-IDF `esp-mqtt`) with Home Assistant discovery (3.7).
- Setup page and JSON API (3.9).
- OTA updates over WiFi. Updating the CC1312 from the ESP (the ESP speaking
  the ROM bootloader protocol itself) is optional and depends on the wiring
  check in [`WIRING.md`](WIRING.md); otherwise the CC1312 is reflashed by hand
  over USB.
- Why ESP-IDF and not ESPHome: devices join at runtime, while ESPHome defines
  its entities at compile time; the protocol code is C either way, and plain
  ESP-IDF keeps it testable on the Mac.

### 4.3 Java bench tool (`host/`)
- Runs M-1 on a Mac with the CC1312 on USB: pair window, `d/pair` with our
  key, `auth/token`, `time`, a host-link capture file and CC1312 stats.
- Its tested CoAP / 6LoWPAN / join code is the reference for the C port on the
  ESP. Not extended beyond M-1, and not deployed.

## 5. Milestones (each is a go/no-go gate)

M1 needs no radio and runs **in parallel** with M-1/M0 from day one.

### M-1 — Prove the thesis: a valve joins with *our* key  *(start first)*
Replicate freedo's on-air join on **one valve**, following the frame-by-frame
spec in [`JOIN.md`](JOIN.md) steps 1-7: button long-press (~2 min
commissioning window), answer its Enhanced Beacon Request (2.4-7.4 ms, no
train), answer its Router Solicitation with the doubled Router Advertisement,
send `d/pair` with our own tag-`0x12` network key behind a full 254-frame
wake-up train, and see the valve answer `2.04` under the new key. CC1312 on
USB, driven by the Java bench tool; this is a thesis test, not a stack.

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
**Works on air (2026-11-17)** with the wireless temperature sensor: Enhanced ACKs
taken (no MAC retries), anchored short trains 9/9. Not measured: the ACK's
on-air start (we know when it is posted, not when the RF core starts it), the
correctness of our CSL phase (a free-running counter, like the bridge's), and
the train jitter (delivery inside a +/-40 ms guard proves no +/-100 us figure).

The timing M-1 could skip:
1. Generate a valid (unsecured) Enhanced-ACK starting 0-1 ms after an RX ends,
   with a correct CSL IE; measure the real gap on our own captures.
2. Track each device's CSL phase from its Enhanced-ACKs and land a 21-frame
   (79 ms) short train + Data frame in its window, at < +/-100 us jitter.

**Gate:** all timing is on-chip by design (3.1). freedo's bridge firmware meets
these windows (ESP32 + CC1101), so this is expected to pass; if it does not,
reconsider an ESP32+CC1101 radio (freedo's path).

### M1 — Gateway protocol core  *(no radio; parallel)*
**Done (2026-11-18)**, in [`gateway/core`](../gateway/README.md): every datagram
and document byte for byte against freedo's and the Java host's builders;
freedo's 370 captured frames and all our host-link captures (1082 `RX`, 887
CoAP, 179 ICMPv6, fragments, 435 reports) parse with nothing rejected; the
host-link client tested against the CC1312 firmware itself. Deliberate
departures from the Java host: one always-on session that serves and pairs on
command (Java has a join run and a serve run); FNV-1a 64 for the ETag and the
session id (the Java host follows); after a failed pair window the Router
Advertisement goes once, under the network key.

The ESP gateway's protocol code in C (6LoWPAN, CoAP, tado resources, report
decoding), built and tested on the Mac like `firmware/`'s `make test`. Replay
freedo `testdata` + our own captures through it and match freedo's parsing.
freedo is an oracle, not ground truth: a mismatch is investigated against raw
captures, not auto-fixed to match.

### M2 — First join, end to end, on the ESP
CC1312 wired to the ESP32 (UART + reset). The full [`JOIN.md`](JOIN.md)
sequence, steps 1-12, driven by the gateway — `d/pair` with an ESP-generated
network key, then `auth/token`, RA, `time`, `d/<serial>/config` — until the
device reports temperature and the reading appears on MQTT and in Home
Assistant.

**Gate:** a valve joins the gateway and its temperature shows up in Home
Assistant.

**Built (2026-11-18), not yet on hardware:** the gateway application in
[`gateway/app`](../gateway/README.md) and its ESP-IDF build for the ESP32-S3 in
`gateway/esp` (builds with ESP-IDF v6.1.1). On the Mac, against the CC1312
firmware itself: configuring the CC1312 and configuring it again after it
rebooted, frame counters in reserved blocks, the kept carrier, pairing a
device and then putting it in a room on the setup page (3.9), and a device
report out as Home Assistant MQTT messages. The first hardware test pairs the
sensor again, to the ESP's own key, and assigns it a room on the page.

### M3 — Control writes
One zone with one valve: a setpoint from Home Assistant over MQTT reaches the
valve in its CSL window and is confirmed by its next report. Then boost and
home/away.

**Setpoint built (2026-11-18), not yet on hardware:** the room keeps its
setpoint (in flash, the one copy; the session asks for it), and a new one, from
the climate entity or the setup page, goes to the room's device as `PUT
z/s?id=<n>` on its channel sample, repeated until it is ACKed (`2.04`, or
`2.05` as a VA01 answers, freedo `PROTOCOL.md`). That write also ends an
overlay set on the device. One write per zone waits on the CC1312 at a time:
a burst of changes (a slider, an automation) costs the device one wake-up,
with the newest value; the same value again wakes it only to end an overlay.
A setpoint is saved to flash 30 s after the last change. The device's echo of the zone state is what Home
Assistant shows. Tested on the Mac through the CC1312 firmware; the sensor
took the same write on air (`SENSOR.md`).

**Boost and home/away built (2026-11-18), not yet on hardware**, in the
shapes freedo writes (`tools/golden` checks our blocks against freedo's
builders byte for byte):
- Home and away is one state of the whole home (`6160`), from the Away switch
  in Home Assistant (`tado/home/set`) or the page. In away each room follows
  its away temperature (`61e0=1 6200`), or is off (`61e0=0`, no `6200`) when
  it has none. The switch restates every room; a write that moves a room's
  target carries `6440=0001`, and an overlay running in a room is kept.
- Boost is the room's climate preset: a timer overlay (`6240=3`) at 25 C for
  30 minutes, kept by the gateway, then the return to the program without
  `6440`; a cancel carries `6440`. A setpoint change also ends an overlay, as
  a cancel.
- Not covered by any capture: an overlay set while away, and how a VA02
  answers a boost.

### M3.5 — Outages
Prove the failure domains of 3.2 on a joined valve: WiFi and broker drops, an
ESP OTA update and an ESP reset while the valve stays joined, and the ESP
resetting a hung CC1312. Settle remote CC1312 updates (wiring check in
[`WIRING.md`](WIRING.md)) or document the manual route.

### M4 — Rooms, boiler, scale
The wall thermostat (after its M-1 check). One room with the thermostat and two
valves: the valves follow the thermostat's measure, and a zone's demand fires
the boiler through the thermostat (3.8). The setup page (3.9), and a check of
the ESP's flash, heap and CPU headroom. Then all rooms, hot water,
persistence across power loss (3.2), frame-counter floor across reboots (3.5),
airtime budget under realistic load (3.6).

### M5 — Home Assistant polish  *(optional / later)*
Pairing and device removal from Home Assistant, more device features (battery,
window state, child lock), whether schedules should move into the gateway, and
rooms that heat only while the boiler runs (3.8).

## 6. Testing strategy
- **Golden fixtures** from freedo `testdata` + our captures; CCM* test vectors
  (`golden/vectors.txt`, shared by the C and Java tests).
- **Unit tests** for the C protocol core (CoAP / 6LoWPAN / MAC / CCM*) built
  natively on the Mac, against those fixtures.
- **Fake CC1312** host-link endpoint for gateway tests (resets, resync,
  timeouts).
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
- Gateway: ESP32-S3 (ESP32-S3-DevKitC-1, WROOM-1 N16R8: 16 MB flash, 8 MB
  octal PSRAM), ESP-IDF v6.1, C.
- MQTT: Mosquitto (the Home Assistant add-on), Home Assistant MQTT discovery,
  no TLS, broker username and password.
- Persistence: ESP NVS for the network key, counter floor and device table.
- Bench tool: Java 25, Maven (with wrapper).

## 9. Heritage / cross-references
- `CappyT/freedo` (Apache-2.0) — ESP32+CC1101 reimplementation; behavioural
  oracle. Key facts used: pairing key `"tado pairing key"` (TLV tag `0x12`
  plaintext vs `0x07` cloud-KEK-wrapped), CCM* parameters, CSL / Enhanced-ACK
  timing, commissioning via button, resource model.
