# gateway/ — ESP32-S3 gateway (C)

The brain of the deployed coordinator (`../docs/PLAN.md` 4.2). It talks to the
CC1312 over UART with the host link (`../docs/HOSTLINK.md`) and runs 6LoWPAN,
CoAP and the tado resource model; it publishes device state over MQTT with
Home Assistant discovery and takes the pair window from MQTT (setpoints: M3).

## Status

**M1 done (2026-11-18):** the protocol core in `core/`, plain C11 with static
tables, built and tested on the Mac.

**M2 built (2026-11-18), not yet run on hardware:** the application in `app/`,
also plain C and tested on the Mac against the CC1312 firmware, and its
ESP-IDF build for the ESP32-S3 in `esp/`. OTA (M3.5) and the setup page (M4)
are not in it yet.

```
make test     # 400+ checks; needs ../reference/freedo (git submodule) for the replay tests
```

## core/

| File | What |
|---|---|
| `coap` | CoAP encode/decode (RFC 7252), stable option order, extended deltas |
| `lowpan` | 6LoWPAN IPHC build/parse (RFC 6282), UDP/ICMPv6 checksums |
| `reasm` | RFC 4944 fragment reassembly in fixed slots |
| `tado` | device config, zone document, zone state with overlay, Block2 answers, `d/pair`, `auth/token`, `time`, Router Advertisement, 2.04/empty ACK, zone state write; report decoding |
| `session` | the coordinator logic: serves the installation, pairs on command, keeps device overlays and ends them; hands decoded reports and the joined device to the gateway |
| `hlc` | the gateway's side of the host link: frames requests, parses replies and events |
| `link.h` | TX requests and events as the session sees them, independent of the wire |

Everything is sans-IO: time comes in as `now_ms`, and TX, logging, random
bytes and wall-clock time go through callbacks.

Shared with the CC1312 firmware, not copied, from `../firmware/src`: `crc`,
`ccm`, `mac`, `bytes.h`, and the host link's `hlframe` (framing, and the
receive buffer that finds frames again after noise or a torn start) and
`hlproto.h` (message types, statuses, flag bits), so both ends read the wire
the same way. The ESP build takes only `crc` and `hlframe`: the gateway opens
no frames itself.

## app/

| File | What |
|---|---|
| `gwlink` | the host link over a byte stream: blocking requests (a `TX` returns the CC1312's handle, `HL_BUSY` retried every 100 ms for 8 s), events that arrive meanwhile queued with their own copy of the bytes |
| `gateway` | configures the CC1312, and again whenever it rebooted (a `boot` line after our `CONFIG`, or a `TX` refused as not configured); frame counters in blocks of 65536, the floor in flash moved on before the CC1312 gets near it; the carrier the CC1312 retunes to; the pair command (`tado/gateway/pair/set`, a zone number), whose joiner becomes that zone's owner; reports out to MQTT |
| `ha` | Home Assistant topics and payloads: retained state per device and zone, a discovery config per sensor once it has a value |

The ESP-IDF code in `esp/main/main.c` gives the gateway its platform: the UART,
NVS, the clock (no `time` answer before SNTP set it), WiFi and MQTT. MQTT
messages reach the gateway task through a queue. The gateway publishes what
changed once per loop, after the devices were answered, with
`esp_mqtt_client_enqueue`, so the broker does not hold up an answer.

## esp/ — build and flash

ESP-IDF v6.1 (`esp-mqtt` is a managed component since v6.0, fetched on the
first build). Wiring: `../docs/WIRING.md`.

```
. ~/esp/esp-idf-v6.1.1/export.sh
cd esp
cat > sdkconfig.secrets <<'EOF'
CONFIG_TADO_WIFI_SSID="..."
CONFIG_TADO_WIFI_PASSWORD="..."
CONFIG_TADO_MQTT_URI="mqtt://192.168.1.10"
CONFIG_TADO_MQTT_USERNAME="..."
CONFIG_TADO_MQTT_PASSWORD="..."
EOF
idf.py set-target esp32s3   # once; reads sdkconfig.defaults and sdkconfig.secrets
idf.py build flash monitor  # the board's "UART" USB port
```

`sdkconfig.secrets` and the generated `sdkconfig` hold the credentials and stay
out of git. After changing `sdkconfig.secrets`, delete `sdkconfig` so it is read
again.

On first boot the gateway mints its own identity (coordinator EUI, network key,
home id) into NVS, and every device must be paired to it. To keep the devices
already joined to the Java bench tool, flash its identity instead, before the
first boot:

```
python tools/identity_nvs.py ../../host/state/coordinator.properties identity.csv --zone 11=<owner EUI>
python -m esp_idf_nvs_partition_gen generate identity.csv identity.bin 0x10000
esptool.py --chip esp32s3 write_flash 0x9000 identity.bin
rm identity.csv identity.bin   # they hold the network key
```

NVS is not encrypted: anyone with the board can read the network key from
flash. NVS is never erased by the firmware (a failed `nvs_flash_init` stops it
instead), as erasing it would lose the key.

Pair a device into zone 3: publish `3` to `tado/gateway/pair/set`, then
long-press the device's button within 120 s. Progress goes to
`tado/gateway/log`.

## Tests

- `golden/vectors.txt` (freedo's builders) and `golden/host_vectors.txt` (the
  Java host's): every datagram and document is checked byte for byte. The
  Java host is the spec; regenerate its vectors with
  `cd ../host && ./mvnw test -Dtest=HostVectorsTest -Dgolden.write=true`.
- freedo's captured frames (`../reference/freedo/tado/testdata`): all 370 open
  and parse, fragmented zone documents reassemble, corrupt frames fail the MIC.
- `test/test_session.c`: the Java `JoinSessionTest` cases, one for one, plus
  what only the C session does (a pair window while serving, a refused
  `d/pair`, reports and the joined device handed to the gateway, no `time`
  before the clock is set).
- `test/test_hlc.c`: the host-link client against the real CC1312 firmware
  (`coordinator.c` and `hostlink.c` built for the Mac with the stub radio of
  `test/fw_peer.c`), including a device report end to end: sealed frame in,
  `RX` event, the session's 2.04 out as a `TX` the firmware seals for the
  device.
- `test/test_gateway.c`: the gateway against the same firmware over a byte
  stream: configuring, reboots, counter blocks, the carrier, a pair command
  through to the joiner owning its zone, and the sensor's report out as Home
  Assistant messages.
- `test/test_ha.c`: discovery configs and state payloads byte for byte.
