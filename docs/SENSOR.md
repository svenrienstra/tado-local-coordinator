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
| `GET z/s?lid=11` | `2.05`: its state of zone 11, kept from the tado days: `6160`=1 home, `6020`=1 heating, `61e0`=1 on, `6200`=1800 (18.00 C), `6240`=2 manual |
| `PUT z/s?id=11` `6160 6180 6020 61e0 6200 6240 62e0 6440` | `2.04`; `6200`=2100 read back as 21.00 C, `6240`=0 |
| `GET d/config`, `GET d/fw`, `GET d/sched` | `4.05`: not readable |
| `GET d/lock` | `4.04`: no child lock |
| `GET z/s` (no query) | `4.00` |

Values are big-endian; a version is `major<<6 | minor`. The firmware is the same
215 family as freedo's valves and wall thermostat.

## Open

- Whether the display shows a written setpoint (it shows the zone's setpoint
  when the user sets one on the sensor).
- What to answer `GET d/<serial>/config`, and the zone document that makes it
  the measuring device of a zone.
