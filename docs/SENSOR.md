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

## Config and zone (tried 2026-11-17)

freedo does not support the sensor (no capture of one; its serial pattern even
skips `SU`), so the documents follow how its valves and wall thermostat take
them (`../reference/freedo/tado/emu.go` `servedConfigGet`, `build_resources.go`,
`build_zone_config.go`; `PROTOCOL.md` 489-618 and 1014-1263). Served by
`serve --zone 11=<sensor EUI>` (`tadocoord.tado.ConfigDocuments`).

1. **`GET d/<serial>/config`** (Block2 0/0/128, with the ETag it kept from tado)
   gets an ACK `2.05` with our ETag, Block2 and, as TLVs (2-byte tag, 1-byte
   length, big-endian value, at most 8 bytes): `0143`=00, `015c`=our home id
   (u32, not 0), `02b3`=00, `015e`=`0d0b` (elected owner of zone 11), `0158`=
   `0200`. Left out: `015d` (interface module id, unknown for a sensor and
   harmful when wrong on valves and thermostats), `0140`, `0149`, `021a`,
   `015a`. It asked twice, then stopped asking.
2. It then **GETs `h/<home>/z/11/config`**: the zone document with it as owner
   and measuring endpoint (`6020`=1, `63e0`=1, `63a0`/`8000`=`<sensor>/z/s`,
   `8400`/`6040`=`<sensor>/z/p`, PID 1000/500/1500, `60e0`=1, `62c0`/`6380`=900;
   URIs `coap://[fe80:0:0:0:<EUI-64, U/L bit flipped>]/z/<res>`). Asked for in
   128-byte blocks, it **took 64-byte ones**: it asked for blocks 1, 2 and 3 in
   turn and stopped at the last. That keeps every block in one frame.
3. Then `PUT d/<serial>/err` (`01a3`=0) and `d/<serial>/fallback` (`0182`=0),
   as a valve reports after a new config, and **`GET h/<home>/z/11/s`**, the
   zone state: we answer `6160`=1 `6180`=0 `6020`=1 `61e0`=1 `6200`=<setpoint>
   `6240`=0 `62e0`=0.
4. Then it reports as the zone's owner: `PUT d/<serial>/sen` (`0162` battery
   4175 mV, `012d` 18.66 C, `012e` 18.67 C, `0135` 56.5 %, `0136`=4),
   `PUT h/<home>/z/11/act` with `40a0` = its heating demand, `z/p` with the same
   demand, and `PUT z/s` echoing the zone state.
5. **It runs the zone's controller.** At 18.00 C against 18.66 measured it
   reported 0 %. A `PUT z/s?id=11` with 21.00 C: `2.04`, then within 1.8 s
   `h/<home>/z/11/act` `40a0`=100 % and `z/p` with 100 %, and the display showed
   21. Set back to 18.00 the same way.

6. **A setpoint set on the sensor itself** (21, at 16:01) is a device overlay,
   as freedo documents for a valve's wheel: `PUT h/<home>/z/11/overlay` with
   `6280`=2100, `6240`=2 (manual), `6260`=1 (on), then `h/<home>/z/11/act`
   `40a0`=94 % and `z/p` with 94 % within 1.4 s. The host answers `2.04` but
   does not keep it yet, so our zone state (`h/<home>/z/11/s`) still says 18.00
   with no overlay; keeping overlays is M3 (the setpoint in Home Assistant
   following the device).

No motor, so nothing to home. A wrong document is fixed by serving a corrected
one under a new ETag, or `POST d/reboot` (it keeps its key); the last resort is
pairing it again.
