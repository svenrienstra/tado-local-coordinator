# gateway/ — ESP32 gateway (C)

The brain of the deployed coordinator (`../docs/PLAN.md` 4.2). It talks to the
CC1312 over UART with the host link (`../docs/HOSTLINK.md`) and runs 6LoWPAN,
CoAP and the tado resource model; it will publish device state over MQTT with
Home Assistant discovery and take setpoints and the pair window from MQTT.

## Status

**M1 done (2026-11-18):** the protocol core in `core/`, plain C11 with static
tables, built and tested on the Mac. The ESP-IDF application around it (UART,
NVS, WiFi, MQTT, SNTP, OTA) is M2.

```
make test     # 300+ checks; needs ../reference/freedo (git submodule) for the replay tests
```

## core/

| File | What |
|---|---|
| `coap` | CoAP encode/decode (RFC 7252), stable option order, extended deltas |
| `lowpan` | 6LoWPAN IPHC build/parse (RFC 6282), UDP/ICMPv6 checksums |
| `reasm` | RFC 4944 fragment reassembly in fixed slots |
| `tado` | device config, zone document, zone state with overlay, Block2 answers, `d/pair`, `auth/token`, `time`, Router Advertisement, 2.04/empty ACK, zone state write; report decoding |
| `session` | the coordinator logic: serves the installation, pairs on command, keeps device overlays and ends them |
| `hlc` | the gateway's side of the host link: frames requests, parses replies and events |
| `link.h` | TX requests and events as the session sees them, independent of the wire |

Everything is sans-IO: time comes in as `now_ms`, and TX, logging, random
bytes and wall-clock time go through callbacks. The session's `tx` returns the
CC1312's handle, so the M2 glue sends the `TX` request through `hlc` and waits
for its reply before returning; events that arrive meanwhile are queued and fed
to the session afterwards (`hlc.h` has the callback rules).

Shared with the CC1312 firmware, not copied, from `../firmware/src`: `crc`,
`ccm`, `mac`, `bytes.h`, and the host link's `hlframe` (framing, and the
receive buffer that finds frames again after noise or a torn start) and
`hlproto.h` (message types, statuses, flag bits), so both ends read the wire
the same way.

## Tests

- `golden/vectors.txt` (freedo's builders) and `golden/host_vectors.txt` (the
  Java host's): every datagram and document is checked byte for byte. The
  Java host is the spec; regenerate its vectors with
  `cd ../host && ./mvnw test -Dtest=HostVectorsTest -Dgolden.write=true`.
- freedo's captured frames (`../reference/freedo/tado/testdata`): all 370 open
  and parse, fragmented zone documents reassemble, corrupt frames fail the MIC.
- `test/test_session.c`: the Java `JoinSessionTest` cases, one for one, plus
  what only the C session does (a pair window while serving, a refused
  `d/pair`).
- `test/test_hlc.c`: the host-link client against the real CC1312 firmware
  (`coordinator.c` and `hostlink.c` built for the Mac with stub radio hooks),
  including a device report end to end: sealed frame in, `RX` event, the
  session's 2.04 out as a `TX` the firmware seals for the device.

Wiring to the CC1312 board: `../docs/WIRING.md`.
