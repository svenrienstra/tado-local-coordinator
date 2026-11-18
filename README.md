# tado-coordinator

A clean-room, local coordinator for tado V3+ 868 MHz heating devices (radiator
valves, wall thermostats, the wireless temperature sensor), built from scratch
around a **TI CC1312R1** radio and an **ESP32-S3** gateway that speaks MQTT to
Home Assistant. No tado cloud, no HomeKit, no key extraction.

> **Status (2026-11-18).** The CC1312 firmware works on air: a tado wireless
> temperature sensor joined our network with our own key, and stays served
> (Enhanced ACKs, reports, `time`, replies on its channel sample). The gateway
> software for M2 and M3 is written and tested on the Mac against that
> firmware: pairing, rooms, Home Assistant climate entities, setpoints, boost
> and home/away. It has not run on an ESP32-S3 yet; that board is the next
> step. Expect bugs, and keep a way to control the heating by hand. See
> [`docs/PLAN.md`](docs/PLAN.md).

## Why this can work without recovering any key

A coordinator does not *read* a device's key — it *mints* one. When a
factory-reset device joins **this** coordinator, we hand it a network key of our
own choosing in the `d/pair` write (plaintext TLV tag `0x12`, secured under the
well-known pairing key `"tado pairing key"`). The device adopts it and reports
under it. The cloud-only per-device KEK (TLV tag `0x07`) that blocks passive key
recovery is simply never involved. The trade-off is inherent: a device joined to
this coordinator **leaves the tado bridge/app/cloud** (one coordinator per
device), and joining needs a button long-press on the device.

## Topology

```
[valves / wall thermostat / sensor]
   |  868 MHz CSL  (hard real-time)
[CC1312]   PHY + MAC + CSL + CCM* + keepalive autonomy   <- all timing lives here
   |  UART (host link)
[ESP32-S3] gateway: 6LoWPAN + CoAP + tado resources + rooms + MQTT + setup page
   |  WiFi, MQTT
[MQTT broker] -> [Home Assistant]
```

The CC1312 and the ESP32-S3 form one self-contained box. Pair devices and put
them in rooms on the gateway's small setup page; from then on Home Assistant
drives the heating: each room is a climate entity (setpoint, boost, the
gateway's target and whether the device confirmed it), each device has its
sensors, and an Away switch covers the whole home.

## Thanks to freedo

This project stands on [`CappyT/freedo`](https://github.com/CappyT/freedo)
(Apache-2.0), an open local coordinator for these devices. Without its work,
and the captures and firmware reading behind it, this would have taken far
longer or not happened at all. What we took from it:

- **The protocol.** freedo's `docs/PROTOCOL.md` is our reference for nearly
  everything on the air: the PHY settings, the CCM* sealing, CSL wake-up
  trains, the join (`d/pair` with our own key, `auth/token`, `time`, the Router
  Advertisement), the device and zone documents, the tag tables, and the
  shapes of setpoint, boost, overlay and home/away writes.
- **A test oracle.** `tools/golden` runs freedo's own builders to produce
  `golden/vectors.txt`, and our C code must match them byte for byte. freedo's
  captured frames (`tado/testdata`) are replayed through our parsers.
- **Design.** The setup page's room tiles take their cue from freedo's
  dashboard.

freedo is a behavioural oracle, not ground truth: where our own captures
disagree, the device wins (see `docs/SENSOR.md`). We reuse no freedo code in
what we ship; if we ever do, its licence and NOTICE come along.

## How this differs from freedo

| | freedo | tado-coordinator |
|---|---|---|
| Radio | CC1101 on an ESP32, running freedo's bridge firmware | TI CC1312R1, with our own firmware |
| Where the timing lives | the ESP32 bridge | the CC1312: Enhanced ACKs, CSL trains and autonomy keep running while the ESP32 restarts |
| Coordinator | a Go daemon, as a Home Assistant add-on or a container | C on the ESP32-S3 itself: one box, no server, no container |
| Home Assistant | its own integration over the daemon's HTTP API | MQTT discovery: climate entities, sensors and an Away switch, through any MQTT broker |
| Daily control | freedo's dashboard, with schedules, or Home Assistant | Home Assistant: schedules and presence are Home Assistant automations |
| Setup | the dashboard | a small page on the gateway: pair a device, then put it in a room |
| Scope today | a running plant: schedules, hot water, the boiler, windows, child lock, multi-device rooms and much more | early: one device per room, setpoints, boost and home/away; the rest follows in later milestones |

If you want something that runs a whole house today, use freedo. This project
explores a self-contained, MQTT-first design with all real-time work on a radio
chip built for it.

## Repository layout

| Path          | Tier                | Language | Notes |
|---------------|---------------------|----------|-------|
| `firmware/`   | CC1312 MAC engine   | C        | PHY/MAC/CSL/CCM*, real-time, autonomous |
| `gateway/`    | ESP32-S3 gateway    | C        | tado protocol, rooms, MQTT, setup page; ESP-IDF build in `gateway/esp` |
| `host/`       | Java bench tool     | Java     | ran M-1 on a Mac; reference for the gateway |
| `tools/`      | test vectors, sniffing | Go, Python | `tools/golden` builds the vectors from freedo |
| `reference/`  | specs & oracles     | -        | freedo, as a git submodule |
| `docs/`       | plan & design       | -        | start at `docs/PLAN.md`; join spec `docs/JOIN.md`, wiring `docs/WIRING.md` |

## Legal

tado is a trademark of its owner, used here only to describe the devices this
interoperates with. Not affiliated with tado. For use with devices you own.
