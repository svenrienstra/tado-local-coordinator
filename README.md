# tado-coordinator

A clean-room, local coordinator for tado V3+ 868 MHz heating devices (radiator
valves, wall thermostats), built from scratch around a **TI CC1312R1** radio and
an **ESP32** gateway that speaks MQTT to Home Assistant. No tado cloud, no
HomeKit, no key extraction.

> Status: **M-1 and M0 pass on air** (2026-11-17). A tado wireless temperature
> sensor joined our network with our key, and the CC1312 firmware plus the Java
> bench tool keep it served: Enhanced ACKs, reports, `time`, and replies behind
> short trains on its channel sample. See [`docs/PLAN.md`](docs/PLAN.md).

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
[valves / wall thermostat]
   |  868 MHz CSL  (hard real-time)
[CC1312]   PHY + MAC + CSL + CCM* + keepalive autonomy   <- all timing lives here
   |  UART (host link)
[ESP32]    gateway: 6LoWPAN + CoAP + tado resources + MQTT   <- updates over WiFi
   |  WiFi / LAN, MQTT
[MQTT broker] -> [Home Assistant]
```

The CC1312 and the ESP32 form one self-contained device. Device state goes out
as MQTT events (temperature, setpoint, battery), and commands such as a new
setpoint come back the same way.

## Repository layout

| Path          | Tier                | Language | Notes |
|---------------|---------------------|----------|-------|
| `firmware/`   | CC1312 MAC engine   | C        | PHY/MAC/CSL/CCM*, real-time, autonomous |
| `gateway/`    | ESP32 gateway       | C        | tado protocol, device state, MQTT (not started) |
| `host/`       | Java bench tool     | Java     | runs M-1 on a Mac; reference for the gateway |
| `reference/`  | specs & oracles     | -        | freedo as behavioural oracle |
| `docs/`       | plan & design       | -        | start at `docs/PLAN.md`; join spec `docs/JOIN.md`, wiring `docs/WIRING.md` |

## Heritage

The PHY (~868.3 MHz, 50 kbps 2-GFSK, 25 kHz dev, sync `0xD391`, 1-byte length), the link crypto
(AES-CCM*), and the pairing-key handshake follow
[`CappyT/freedo`](https://github.com/CappyT/freedo) (an ESP32+CC1101
reimplementation, Apache-2.0), cross-checked against our own captures. freedo's
`tado/` package is the **behavioural spec and test oracle**; if we reuse its code,
we keep its licence and NOTICE.

tado is a trademark of its owner; used here only to describe the devices this
interoperates with. For use with devices you own.
