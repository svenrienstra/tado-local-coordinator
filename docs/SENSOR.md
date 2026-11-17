# The wireless temperature sensor on our network

What the tado wireless temperature sensor did on our coordinator (bench, Java
host, 2026-11-17). Requests were sent with `tadocoord.bench.DeviceRequest`;
answers are the device's, decoded with freedo's tag tables (`PROTOCOL.md`).

## Pairing

- A long press or the pairing-reset menu entry did not make it leave the
  commercial network: it kept its old key and E0 followed. Removing it in the
  tado app did; it then joined us with `d/pair` (`JOIN.md`).
- Unplug the commercial bridge while pairing, or it answers the Beacon
  Request.

## What it sends

- `PUT d/<serial>/fw/state` after joining, fragmented over two frames.
- `PUT z/p?lid=11` with `4060` (temperature, centi-C), `40a0` (demand) and
  `0135` (humidity, 0.1 %), **twice per update with two MIDs**: not a
  retransmission.
- `GET d/<serial>/config` about once a minute, retried under one MID, while
  unanswered.
- `GET time` after joining.
- A Router Solicitation every 3 s once it has heard no Router Advertisement
  for about 20 minutes; it then stops reporting. The host sends an RA every
  200-600 s.
- It samples the channel every 1 s and sends its uplinks right after a sample,
  so an anchored reply lands about 1 s later.

## What it serves

| Request | Answer |
|---|---|
| `PUT d/identify` (empty, Uri-Port ffff, CF 42) | `2.04`; the display shows "HI" |
| `GET d/fw/state` | `2.05`: type `01a0`=8, running firmware `003a`=215.1 (status `0e`: image, active, confirmed), slot 0x10 `0035`=73.1, slot 0x11 `0039`=215.1, build `0210`="1556c5e" |
| `GET z/config`, `GET z/config?lid=11` | `2.05`, empty: no zone document yet |
| `GET z/s?lid=11` | `2.05`: `6160`=1 home, `6020`=1 heating, `61e0`=1 on, `6200`=1800 (18.00 C), `6240`=2 manual. Its room had the heating off under tado, so this is likely the state a reset leaves, not the old one |
| `PUT z/s?id=11` `6160 6180 6020 61e0 6200 6240 62e0 6440` | `2.04`; `6200`=2100 read back as 21.00 C, `6240`=0, and **the display shows the setpoint 21**. Written back to 18.00 the same way |
| `GET d/config`, `GET d/fw`, `GET d/sched` | `4.05`: not readable |
| `GET d/lock` | `4.04`: no child lock |
| `GET z/s` (no query) | `4.00` |

Values are big-endian; a version is `major<<6 | minor`. The firmware is the same
215 family as freedo's valves and wall thermostat.

## Next: config and zone (plan, not yet tried)

freedo does not support the sensor (no capture of one; its serial pattern even
skips `SU`), so this is inferred from how its valves and wall thermostat work
(`../reference/freedo/tado/emu.go` `servedConfigGet`, `build_resources.go`,
`build_zone_config.go`; `PROTOCOL.md` 489-618 and 1014-1263).

1. **Answer `GET d/<serial>/config`.** Today it gets no answer; freedo answers a
   device it holds no document for with `5.03`. A thermostat holds back its other
   uplinks until it gets a `2.05`/`2.03`, which may be why the sensor keeps
   asking. The answer: ACK `2.05` with an 8-byte ETag (option 4) and Block2
   (option 23, 128-byte blocks, M set while more follow), no Content-Format, and
   TLVs (tag 2 B, length 1 B, value BE, each value at most 8 bytes):
   - `0143`=00 (trace off)
   - `015c`= our **home id**, u32, not 0. A device applies nothing with home 0
     and uses it in every `h/<home>/...` path, in decimal.
   - `02b3`=00 (no offline schedule; 1 makes it fetch `h/<home>/z/N/sched`)
   - `015e`= one `[role][zone]` record per zone, the whole table in one transfer:
     `0d0b` makes it the elected owner (bit 3) of zone 11
   - `0158`=`0200` (notify on a remote overlay change)
   - leave out `015d` (interface module id: unknown for a sensor, and a wrong one
     breaks valves and thermostats), `0140` (its own offset), `0149` and `021a`
     (valve only), and `015a` (the ETag never goes in a GET answer).
   A matching ETag in its request gets `2.03` with no body.
2. **Then it should `GET h/<home>/z/11/config`**, the zone document. Make it the
   owner and measuring endpoint: `6020`=1, `63e0`=1, `63a0`=`<sensor>/z/s`,
   `8400`=`<sensor>/z/p`, `8000`=`<sensor>/z/s`, `6040`=`<sensor>/z/p`, PID
   `6080`=1000, `60a0`=500, `60c0`=1500, `60e0`=1, `62c0`=900, `6380`=900. A URI
   is `coap://[fe80:0:0:0:<EUI-64 with the U/L bit flipped, lowercase hex>]/z/<res>`.
   Same ETag/Block2 shape; an unknown zone gets `4.04`.
3. **Watch** for `PUT z/p?lid=11`, `h/<home>/z/11/act` and the other reports a
   valve sends after this, and for the config retries stopping.

Risks: no motor to home on a sensor. A wrong document is fixed by serving a
corrected one under a new ETag, or `POST d/reboot` (it keeps its key); the last
resort is pairing it again.
