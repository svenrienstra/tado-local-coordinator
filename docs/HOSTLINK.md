# Host link — CC1312 <-> host wire protocol (v0)

The framed protocol of `PLAN.md` 3.4, version 0. The host is the Java bench
tool over USB serial for M-1 and the ESP32 gateway over a direct UART from M2.
Little-endian throughout. The link is not encrypted (`PLAN.md` 3.3).

## Framing

```
0x7E 0xA5 | len (u16) | body (len bytes) | crc (u16)
body = seq (u8) | type (u8) | payload
```

- `crc` is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection) over
  `len` and `body`.
- `len` is at most 300. A receiver that sees a bad length or CRC drops one byte
  and searches for the next `7E A5` (resync).
- UART: 460800 baud, 8N1, no flow control.

## Requests and replies

The host sends requests with its own `seq`. The CC1312 answers every request
exactly once with `type | 0x80` and the same `seq`; the first payload byte of a
reply is a status:

| Status | Meaning |
|---|---|
| `0x00` | OK |
| `0x01` | bad length / bad argument |
| `0x02` | not configured (send `CONFIG` first) |
| `0x03` | busy (TX queue full) |
| `0x04` | unknown type |
| `0x05` | unsupported silicon: `CONFIG` on a CC13x2 older than rev E (PG2.1), which lacks the per-frame frequency offset the RX path reads |

A request identical to the previous one (same `seq` and same CRC) is a
retransmission: the CC1312 re-sends the cached reply and does not execute it
again. A request that reuses the previous `seq` with other content (a restarted
host) is executed. The host resends
a request whose reply does not arrive in 200 ms, up to 3 times.

Events from the CC1312 are unsolicited, carry the CC1312's own `seq`, and get no
reply (v0 accepts loss of an event; RX frames are retransmitted by devices).

## Requests (host -> CC1312)

| Type | Name | Payload | Reply payload after status |
|---|---|---|---|
| `0x01` | `PING` | - | `fw_version` (u16), `rat` (u32, 4 MHz radio timer) |
| `0x02` | `CONFIG` | `coord_eui` [8] (wire order; must end in `c5 1b 00`), `network_key` [16], `counter_floor` (u32), `channel_khz` (u32, e.g. 868335) | - |
| `0x03` | `PAIR_WINDOW` | `seconds` (u16; 0 closes) | - |
| `0x04` | `TX` | `flags` (u8), `dst_eui` [8] (ignored for broadcast), `packet` (6LoWPAN datagram) | `handle` (u8), `mac_seq` (u8), `counter` (u32) |
| `0x05` | `STATS` | - | counters, u32 each, in the order below |

`TX` flags:

| Bit | Meaning |
|---|---|
| 0 | secure under the **pairing** key (else the network key) |
| 1 | broadcast (FCF `49e8`, dst `ffff`) instead of unicast (FCF `69ec`) |
| 2 | Frame Pending (`79ec` / `59e8`) |
| 4-5 | wake-up train: 0 none, 1 full (254 frames, 1.003 s), 2 anchored: a short train (21 frames, 79 ms) centred on the device's next channel sample, which the CC1312 learns from the phase in the device's Enhanced ACKs; a full train while it knows none or after a missed ACK. An anchored job waits in the queue for that sample, up to about 1 s |

The CC1312 allocates `mac_seq` and `counter`, wraps the packet (unicast:
`coord_eui[5:8]` `04` `counter` packet FCS; broadcast: `counter >> 16` (u16)
packet FCS, with `counter & 0xffff` in the header), seals it with CCM*, and
queues it. A unicast frame expects an Enhanced ACK; the outcome comes as a
`TX_DONE` event. A `CONFIG` retunes the radio and so aborts a frame on air: it
ends with `TX_DONE` radio error, as does a frame the radio never reports done
(after its airtime plus 50 ms).

`STATS` counters (since boot, wrapping; a host ignores counters past the ones it
knows, so firmware may append new ones):

| # | Counter | Counts |
|---|---|---|
| 0 | `rx_ok` | frames the RF core received with a good CRC |
| 1 | `rx_crc_fail` | frames the RF core dropped for a bad CRC (`nRxNok`) — a high value with no `rx_ok` points at the PHY settings |
| 2 | `rx_unverified` | good-CRC frames that opened under no key, or were not ours |
| 3 | `beacons_answered` | Enhanced Beacons sent |
| 4 | `acks_sent` | Enhanced ACKs accepted by the radio |
| 5 | `answers_refused` | beacons/ACKs the radio refused (a job was on air) |
| 6 | `jobs_ok` | TX jobs that completed (acked or sent) |
| 7 | `jobs_failed` | TX jobs that ended in no ACK or a radio error |
| 8 | `jobs_timed_out` | TX jobs failed by the on-air watchdog |
| 9 | `radio_errors` | RF command errors (each also sent as a `LOG`) |
| 10 | `link_crc_errors` | host-link frames dropped for a bad CRC |
| 11 | `link_resyncs` | bytes skipped while searching for a frame start |
| 12 | `acks_late` | Enhanced ACKs posted after their start time (sent late; the device may not take them) |
| 13 | `jobs_anchored` | TX jobs sent behind a short train on the device's channel sample |
| 14 | `rx_duplicates` | unicasts that repeat the last one from their source (same MAC seq, within 30 s): ACKed, not sent as `RX` |

## Events (CC1312 -> host)

| Type | Name | Payload |
|---|---|---|
| `0xC0` | `LOG` | ASCII text: `boot fw=<n> reset=<reason>` once after reset, then one line per radio error, refused answer, late ACK or failed job; `carrier offset <Hz> over <n> devices` per 16 carrier readings from our devices, and `retuned to <kHz> kHz (<k> from CONFIG)` when the radio follows them (the host keeps that frequency for the next `CONFIG`; the CC1312 stores nothing) |
| `0xC1` | `RX` | `flags` (u8), `rssi` (i8), `mac_seq` (u8), `src_eui` [8], `counter` (u32), `rat` (u32), `packet` |
| `0xC2` | `BEACON_ANSWERED` | `joiner_eui` [8], `request_counter` (u32), `rssi` (i8), `beacon_seq` (u8) |
| `0xC3` | `RX_UNVERIFIED` | `rssi` (i8), `rat` (u32), `frame` (raw MAC frame, no PHY CRC) |
| `0xC4` | `TX_DONE` | `handle` (u8), `result` (u8): 0 acked, 1 sent (no ACK expected), 2 no ACK, 3 radio error |

`RX` flags: bit 0 = verified under the pairing key (else network key), bit 1 =
broadcast, bit 2 = an Enhanced ACK was sent. `packet` is the 6LoWPAN datagram
with the tado shim (`c51b00 04 counter`) and the MAC FCS removed.

`RX_UNVERIFIED` reports CRC-valid frames that verify under no key (or are not
Data/broadcast/beacon frames, e.g. wake-ups and foreign ACKs). Debugging only;
the CC1312 may rate-limit it.

## Behaviour on the CC1312

- **Beacon Requests** (`0be8`) that verify under the pairing key, while a pair
  window is open, get an Enhanced Beacon 2.4-7.4 ms after the request ends, with
  no train; then a `BEACON_ANSWERED` event.
- **Unicast Data** (`69ec`/`79ec`) addressed to `coord_eui` is verified under the
  network key, then (pair window open) under the pairing key. A verified frame
  gets an unsecured Enhanced ACK (best effort in v0; the 0-1 ms gap is M0) and an
  `RX` event. An ACK carries CSL phase = RAT / 1280 (320 us units, 16 bits) and
  period 3125.
- **Broadcast Data** (`49e8`/`59e8`) from devices is verified the same way and
  reported as `RX` with the broadcast flag; no ACK.
- **Counters** start at `counter_floor` from `CONFIG`; v0 does not persist them
  (the host passes a floor that it persists, as freedo does). Flash persistence
  is M4.
