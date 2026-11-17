# host/ — Java bench tool

Runs M-1 on a Mac with the CC1312 on USB: the pair window, `d/pair` with our
own network key, `auth/token` and `time`, over the host link
(`../docs/HOSTLINK.md`). CCM* is decrypt/validate only — the CC1312 owns
outgoing seq/counters and sealing.

It is not deployed. From M2 the ESP32 gateway (`../gateway/`) is the host; this
code is the tested reference for that C port (`../docs/PLAN.md` 4.3).

Build: Maven wrapper, Java 25 (the enforcer rejects an older JDK).

```
./mvnw test      # unit tests; golden vectors from ../golden/vectors.txt
./mvnw package   # target/tado-coordinator-host.jar (runnable, dependencies included)
```

## M-1 / M2 bench run

With the CC1312 flashed and on USB:

```
java -jar target/tado-coordinator-host.jar join \
    --port /dev/tty.usbserial-XXXX --state state --window 120
```

then long-press the valve button. The tool opens a pair window, answers the
valve's Router Solicitations with the doubled Router Advertisement, sends
`d/pair` with our network key behind a full wake-up train, and exits 0 once the
valve answers `2.04` under that key (`../docs/JOIN.md` step 7). It then serves
`auth/token` and `time` for 30 s more.

To keep joined devices served between tests, run

```
java -jar target/tado-coordinator-host.jar serve --port /dev/tty.usbserial-XXXX --state state
```

until Ctrl-C. It opens no pair window. It answers reports (`2.04`), `time`,
`auth/token` and Router Solicitations, and sends an unsolicited Router
Advertisement every 200-600 s, as the bridge does. A device that hears no
advertisement for about 20 minutes stops reporting and only solicits, and it may
not take an advertisement again for a long time.

Every run writes `state/captures/hostlink-<UTC time>.log`: each host-link frame
in both directions, timestamped, with frames the CC1312 could not open decoded
from their MAC header (`data dst=… src=…`, `beacon-request`, `wake-up`, …). The
network key in `CONFIG` is redacted. The console also prints the CC1312's
`LOG` lines and, every 5 s, the `STATS` counters that moved (e.g. a rising
`rx_crc_fail` with no `rx_ok` means the PHY settings do not match the valves).

`state/coordinator.properties` holds the coordinator EUI, the network key and
the frame-counter floor (a block of 4096 is reserved per `join` run, 2^20 per
`serve` run). It is created on
first run with mode 600; keep it — a device joined with that key needs it.

## Layout

| Package | Contents |
|---|---|
| `tadocoord.link` | host-link framing, events, `HostLink` client, serial transport |
| `tadocoord.lowpan` | 6LoWPAN IPHC datagrams, IPv6 checksums |
| `tadocoord.coap` | CoAP message codec |
| `tadocoord.tado` | join resources: `d/pair`, `auth/token`, `time`, Router Advertisement |
| `tadocoord.join` | `JoinSession`: the join state machine, and the serving mode behind `serve` |
