# tado-coordinator

A clean-room, local coordinator for tado V3+ 868 MHz heating devices (radiator
valves, wall thermostats), built from scratch around a **TI CC1312R1** radio and
a **Java** control daemon. No tado cloud, no HomeKit, no key extraction.

> Status: **planning**. Nothing here is built yet. See [`docs/PLAN.md`](docs/PLAN.md).

## Why this can work without recovering any key

A coordinator does not *read* a device's key — it *mints* one. When a
factory-reset device joins **this** coordinator, we hand it a network key of our
own choosing in the `d/pair` write (plaintext TLV tag `0x12`, secured under the
well-known pairing key `"tado pairing key"`). The device adopts it and reports
under it. The cloud-only per-device KEK (TLV tag `0x07`) that blocks passive key
recovery is simply never involved. The trade-off is inherent: a device joined to
this coordinator **leaves the tado bridge/app/cloud** (one coordinator per
device).

## Topology

```
[valves / wall thermostat]
   |  868 MHz CSL  (hard real-time)
[CC1312]   PHY + MAC + CSL + CCM* + local autonomy      <- all timing lives here
   |  UART
[ESP32]    transparent UART <-> TCP (+TLS) bridge        <- dumb pipe, no logic
   |  WiFi / LAN
[container]  Java daemon: CoAP + 6LoWPAN + zones + schedules + HTTP API
   |
[Home Assistant]
```

The radio sits where the RF needs it; the brain runs as a container anywhere on
the LAN. The ESP32 is only a wire, so the brain stays a full JVM.

## Repository layout

| Path          | Tier                | Language | Notes |
|---------------|---------------------|----------|-------|
| `firmware/`   | CC1312 MAC engine   | C        | PHY/MAC/CSL/CCM*, real-time, autonomous |
| `bridge/`     | ESP32 serial bridge | C / YAML | transparent UART<->TCP(+TLS), no logic |
| `host/`       | Java control daemon | Java     | CoAP/6LoWPAN/zones/schedules/API |
| `reference/`  | specs & oracles     | -        | freedo as behavioural oracle; sniffer repo |
| `docs/`       | plan & design       | -        | start at `docs/PLAN.md` |

## Heritage

The PHY (868.300 MHz, 50 kbps 2-GFSK, 25 kHz dev, sync `0x91D3`), the link crypto
(AES-CCM*), and the pairing-key handshake were reverse-engineered in the sibling
`re-lab` project and cross-checked against
[`CappyT/freedo`](https://github.com/CappyT/freedo) (an ESP32+CC1101
reimplementation). freedo's `tado/` package is used here only as a **behavioural
spec and test oracle**; confirm its licence before reusing any code.

tado is a trademark of its owner; used here only to describe the devices this
interoperates with. For use with devices you own.
