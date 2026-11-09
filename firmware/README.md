# firmware/ — CC1312 MAC engine (C)

Real-time radio tier. Owns everything with a deadline: PHY TX/RX (RF core,
RAT-timed), the tado MAC (Data frames, wake-up trains, unsecured Enhanced-ACK,
Enhanced Beacon answer), AES-CCM* (sole encryptor of outgoing link frames; seq
and frame counter allocation), and the host link (`../docs/HOSTLINK.md`).
Later: anchored CSL timing (M0), flash-persisted counters, cached Router
Advertisements and the autonomy layer (M4). See `../docs/PLAN.md`.

Current scope: **M-1** (`../docs/JOIN.md` steps 1-7) on a CC1312R1 board (`../docs/WIRING.md`)
over USB.

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
  override with `DSLITE=...`;
  `CCXML=<your CC1312R1F3 target config>`; UniFlash writes the `.ccxml` when you
  pick "CC1312R1F3" + "Texas Instruments XDS110 USB Debug Probe" and save the
  configuration).
- **ROM serial bootloader:** the CCFG keeps the bootloader enabled, but the
  backdoor pin is off by default because the board's pin pull-ups are not
  known yet. `make clean all BL_PIN=<dio>` turns it on (active low). Then hold
  that pin low through a reset and flash over the UART with
  `cc2538-bsl.py -p <port> -e -w -v build/coordinator.hex`. Whether the ESP32
  can drive this remotely depends on the board wiring (`../docs/WIRING.md`);
  decided at M3.5.

Safety: the CCFG leaves debug access enabled and the bootloader on; never
enable read-out or debug locks.

## Not yet confirmed on hardware

The frames are byte-exact against freedo, but the radio layer has not been on
air yet. To check on the bench first, in this order:

1. PHY match with a real valve: sync word `D391` sent twice (32 bits, freedo's
   CC1101 "30/32" mode), preamble polarity, 1-byte length semantics, and the
   default prop-mode CRC-16 being CC1101-compatible (poly 0x8005, init 0xFFFF).
2. RX bandwidth code `rxBw = 0x55` (~137 kHz intended) and `biasMode`.
3. Frequency: 868.300 MHz from `CONFIG`; freedo's CC1101 sits at 868.324 MHz.
4. RX timestamp: assumed to mark the sync word, so the frame end is
   `ts + (len + 3) x 640` RAT ticks. The beacon (3 ms) and ACK (0.5 ms) delays
   hang off it.
5. Wake-up trains: 254 chained `CMD_PROP_TX` at 4.0 ms (each frame ~3.4 ms on
   air), Data 12 ms after the last; check the spacing on a logic analyser.
6. The Enhanced ACK is best effort: aborting RX and posting the TX chain from
   the main loop may miss the 0-1 ms gap. That is M0's work.
