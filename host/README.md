# host/ — Java control daemon (containerised)

The brain: no hard deadlines. CoAP, 6LoWPAN, the tado resource model,
coordinator logic (pair window, network-key generation, device DB,
zones/schedules, persistence), the framed reconnect-tolerant link to the CC1312
(`../docs/HOSTLINK.md`), and later an HTTP API for Home Assistant. CCM* is
decrypt/validate only — the CC1312 owns outgoing seq/counters and sealing.

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

Every run writes `state/captures/hostlink-<UTC time>.log`: each host-link frame
in both directions, timestamped, with frames the CC1312 could not open decoded
from their MAC header (`data dst=… src=…`, `beacon-request`, `wake-up`, …). The
network key in `CONFIG` is redacted. The console also prints the CC1312's
`LOG` lines and, every 5 s, the `STATS` counters that moved (e.g. a rising
`rx_crc_fail` with no `rx_ok` means the PHY settings do not match the valves).

`state/coordinator.properties` holds the coordinator EUI, the network key and
the frame-counter floor (a block of 4096 is reserved per run). It is created on
first run with mode 600; keep it — a device joined with that key needs it.

## Layout

| Package | Contents |
|---|---|
| `tadocoord.link` | host-link framing, events, `HostLink` client, serial transport |
| `tadocoord.lowpan` | 6LoWPAN IPHC datagrams, IPv6 checksums |
| `tadocoord.coap` | CoAP message codec |
| `tadocoord.tado` | join resources: `d/pair`, `auth/token`, `time`, Router Advertisement |
| `tadocoord.join` | `JoinSession`, the join state machine |
