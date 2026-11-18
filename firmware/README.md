# firmware/ — CC1312 MAC engine (C)

Real-time radio tier. Owns everything with a deadline: PHY TX/RX (RF core,
RAT-timed), the tado MAC (Data frames, wake-up trains, unsecured Enhanced-ACK,
Enhanced Beacon answer), AES-CCM* (sole encryptor of outgoing link frames; seq
and frame counter allocation), and the host link (`../docs/HOSTLINK.md`).
It also keeps CSL anchors: a device's channel sample, learnt from the phase in
its Enhanced ACKs, so that a reply goes behind a short train on that sample.
Later: flash-persisted counters, cached Router Advertisements and the autonomy
layer (M4). See `../docs/PLAN.md`.

Current scope: **M-1 and M0** on a CC1312R1 board (`../docs/WIRING.md`) over USB.

## Layout

| Path | What |
|---|---|
| `src/` | Portable core, no TI headers: `crc`, `ccm` (CCM* over an `aes128_encrypt_block` hook), `mac` (frame builders/parsers), `hostlink` (framing, resync, reply cache), `coordinator` (pair window, key fallback, seq/counter, TX queue) |
| `test/` | Host tests (clang) against `../golden/vectors.txt`, plus a software AES used only there |
| `target/` | CC1312R1: `main.c` (NoRTOS loop), `radio.c` (TI RF driver, prop mode), `rf_settings.c`, `host_uart.c` (UART0 460800), `aes_hw.c` (AES engine), `board.c` (driver config), `ccfg.c`, linker script |

## Build and test

Needs the TI CoreSDK that ships inside a contiki-ng checkout (set
`CONTIKI_NG`, default `../../contiki-ng`) and the Arm GNU Toolchain
(`brew install --cask gcc-arm-embedded`; override `CROSS` for another install).

```sh
make test   # portable core on the host, golden vectors + host link + coordinator
make all    # build/coordinator.hex
```

`golden/vectors.txt` is generated from freedo's builders by `../tools/golden`;
the tests reproduce each frame byte for byte.

## Flash

- **Debugger (XDS110 probe on the board's 10-pin JTAG header):** `make flash` erases, programs,
  verifies and then pin-resets, using UniFlash's DSLite binary (`brew install
  --cask uniflash`, which needs Rosetta; found under `/Applications/TI/UniFlash`,
  override with `DSLITE=...`). The target config is `CC1312R1F3.ccxml` in this
  directory (override with `CCXML=...`): CC1312R1F3 on an XDS110 USB Debug
  Probe, 2-pin cJTAG, with the probe supplying 3.3 V to the target.
- **ROM serial bootloader:** the CCFG keeps the bootloader enabled, but the
  backdoor pin is off by default because the board's pin pull-ups are not
  known yet. `make clean all BL_PIN=<dio>` turns it on (active low). Then hold
  that pin low through a reset and flash over the UART with
  `cc2538-bsl.py -p <port> -e -w -v build/coordinator.hex`. Whether the ESP32
  can drive this remotely depends on the board wiring (`../docs/WIRING.md`);
  decided at M3.5.

Safety: the CCFG leaves debug access enabled and the bootloader on; never
enable read-out or debug locks.

## On air (2026-11-17)

Confirmed with a wireless temperature sensor and against the commercial bridge's
traffic, timed on this radio with a `make SNIFF=1` build:

- PHY: frames in both directions decode; the join, reports, `time` and replies
  work.
- RX timestamp: the RF core dates a packet at its first preamble bit, so the
  frame ends `ts + (preamble 4 + sync 4 + 1 + len + 2) x 640` RAT ticks later. A
  frame reaches the main loop about 0.49 ms after its end.
- Enhanced ACK: sent from the clear header before the MIC check, 0.70 ms after
  the frame; the bridge's starts 0.35-0.45 ms after. Posting it ends about
  0.66 ms after the frame (0.49 ms to reach the main loop, then cancelling RX
  and posting the chain), so an earlier start went out late.
- Trains: wake-up frames every 3968 us (12.4 rendezvous units), the Data frame
  0.52 ms (broadcast) or 0.82 ms (unicast) after the last wake-up frame ends.
  A full train takes 1.05 s. An anchored train is 21 frames, rendezvous 248 to
  0, centred on the device's sample; it reached the sensor 9 times out of 9.

- Frequency: tuned in 5 kHz steps with the bridge on, this radio receives the
  plant cleanly from 868.290 to 868.385 MHz (868.285 and 868.390: almost
  nothing), so the plant sits at 868.335 MHz on our synthesizer, which the host
  now sends in `CONFIG`. 868.300 was 10 kHz from the lower edge.
- Carrier tracking: an RF override (`0x000488E3`, rev E silicon) makes the RF
  core append its estimate of each frame's carrier offset, a signed byte in
  1/64 of the 25 kHz deviation, before the RSSI. It is steady (sd ~0.5 units
  per sender) and agrees with the sweep; its scale reads about 8 % low. The
  coordinator averages it per device over our own devices only, logs the
  median every 16 readings (`carrier offset <Hz> over <n> devices`), and past
  4 kHz retunes the radio (`retuned to <kHz> kHz (<k> from CONFIG)`), at most
  every 2 minutes, at most 40 kHz from `CONFIG`, only while idle. Started at
  868.300 MHz it reached 868.335 after 5 minutes and 868.340 after 15, from
  the sensor alone.

Still open: the RX bandwidth code `rxBw = 0x55` (~137 kHz intended) and
`biasMode`.
