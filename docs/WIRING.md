# Wiring

Two setups. Bench mode (M-1) needs no extra wiring; gateway mode (M2 onward)
adds the ESP32 gateway next to the radio board.

## The radio board

A third-party **CC1312R1** board silkscreened "CC1310 DK V1.3". The silkscreen
is wrong: the chip is marked CC1312 R1F3 and reports itself as a CC1312 rev 2.1.
It carries:

- a micro-USB port feeding an on-board USB-serial chip (the 16-pin SOIC next to
  the port), which shows up as `/dev/tty.usbserial-*` and is wired to DIO2/DIO3
- a 2×5 JTAG box header next to RESET, where the XDS110 probe plugs in with a
  10-pin ribbon cable (back silkscreen: RST, TDI, TDO, TCK, TMS, GND ×3, VDD)
- buttons BTN1, BTN2 and RESET (which DIOs the two buttons use is not known yet)
- a VBAT connector with a charger (the green and red LEDs are its STDBY and
  CHRG lights, not GPIOs)
- an SMA antenna connector and an LCD flat-cable connector
- two 2×8 pin headers that break out the DIOs; their names are printed on the
  back:

| Header (front) | Signals |
|---|---|
| upper right, next to the JTAG header | D0 – D15 |
| lower right, next to the LCD connector | D18 – D30, nRST, TDI (DIO17), TDO (DIO16) |

On the D0–D15 header, the back silkscreen gives pin *n* = DIO *n−1*: the edge
row holds D0, D2, … D14 (pins 1, 3, … 15) and the inner row holds D1, D3, … D15
(pins 2, 4, … 16). Check this with a multimeter before relying on it. Neither
header has a GND pin. Ground is available on the JTAG header and on the VBAT
connector's negative pin.

Pins as the firmware uses them:

| CC1312 pin | Header pin | Use | Set in |
|---|---|---|---|
| DIO2 | upper header, pin 3 | UART0 RX (host → CC1312) | `firmware/target/host_uart.c` |
| DIO3 | upper header, pin 4 | UART0 TX (CC1312 → host) | `firmware/target/host_uart.c` |
| nRST | lower header | reset, active low | — |
| `BL_PIN` (default: none) | — | bootloader backdoor, active low | `firmware/Makefile` |

DIO2/DIO3 are not a free choice. TI's ROM serial bootloader on the CC1312R
always uses UART0 RX on DIO2 and TX on DIO3 ([SWRA466], Table 2-11, CC1312R
column). Keeping the host link on the same pins means one pair of wires carries
both the host link and remote reflashing.

## Bench mode — two USB cables

```
 Mac ──USB──► micro-USB ─► [ USB-serial chip ] ◄──► DIO2 / DIO3 ─┐
              /dev/tty.usbserial-XXXX                            │
                                                     [ CC1312R1 board ]
 Mac ──USB──► [ XDS110 probe ] ══10-pin ribbon══► JTAG header ───┘
              make flash (DSLite + CC1312R1F3.ccxml)
```

The Java host opens the USB-serial port. `make flash` goes through the XDS110.
Both work with the default build.

## Gateway mode — ESP32-S3 next to the board

The gateway is an **ESP32-S3-DevKitC-1 with an ESP32-S3-WROOM-1 N16R8** (16 MB
flash, 8 MB octal PSRAM). The pins are set in `gateway/esp/main/Kconfig.projbuild`.

```
     ESP32-S3-DevKitC-1 (N16R8)                 CC1312R1 board
   ┌──────────────────────┐                ┌───────────────────────────┐
   │ GPIO17  (UART1 TX) ──┼──────────────► │ DIO2  (upper header pin 3) │
   │ GPIO18  (UART1 RX) ◄─┼─────────────── │ DIO3  (upper header pin 4) │
   │ GPIO4   open-drain ──┼──────────────► │ nRST  (lower header)       │
   │ GPIO5   open-drain ──┼──────────────► │ BL_PIN (BTN1's DIO)        │
   │ GND ─────────────────┼─────────────── │ GND (JTAG header or VBAT−) │
   └──────────┬───────────┘                └─────────────┬─────────────┘
              │ USB 5 V ("UART" port)                    │ micro-USB 5 V
          [ power ]                                  [ power ]
```

Pins to leave alone on an N16R8: GPIO26-32 (the module's flash), GPIO33-37
(its octal PSRAM), GPIO19/20 (USB), GPIO43/44 (UART0, the console on the
"UART" USB port), and the strapping pins GPIO0, 3, 45 and 46. GPIO48 (or 38 on
board v1.1) drives the RGB LED. The S3 has no GPIO22-25. GPIO5 is wired for
remote reflash and not used by the gateway yet.

- **Cross the UART lines:** ESP TX goes to CC1312 RX (DIO2), and ESP RX comes
  from CC1312 TX (DIO3). The UART runs at 460800 8N1. The bootloader detects
  the baud rate itself (up to 1.6 Mbaud), so 460800 also works for reflashing.
- **DIO2 is shared with the USB-serial chip, and the board is not modified.**
  The chip's TX output also drives DIO2, so this wiring only works if a series
  resistor of at least about 1 kΩ sits between them. Then the ESP, wired
  straight to DIO2, wins: the most current that can flow through the resistor
  is 3.3 V / 1 kΩ = 3.3 mA. DIO3 is fine to share, because both only listen to
  it. Don't send anything from the Mac over the USB-serial port while the ESP is
  connected. If the measurement shows a direct trace or a small resistor, use
  the alternative below instead.
- **Backdoor pin:** BTN1 is the natural choice if, as on TI's LaunchPads, it
  pulls a DIO to ground. Find its DIO with a continuity check from the button
  pad to the D0–D15 header. On TI's CC13x2 LaunchPads BTN-1 is DIO15, but this
  board need not match. Errata IOC_01 only rules out the very highest DIO
  numbers ([SWRZ078]), so any pin on the D0–D15 header is allowed. Build with
  `make clean all BL_PIN=<dio>`. The backdoor is off unless `BL_PIN` is set, so
  a wrong guess cannot leave the board stuck in the bootloader.
- **Drive GPIO4 and GPIO5 as open-drain** (pull low or release, never drive
  high). The RESET button and BTN1 also pull those nets to ground, and while the
  ESP boots its pins float, which reads as released.
- **Share ground.** Both sides use 3.3 V logic, so no level shifter is needed.

### Alternative: reflash by hand

If the USB-serial chip drives DIO2 directly (or through less than about 1 kΩ),
skip remote reflash instead:

- A firmware build option moves UART0 to two free DIOs (to be added when
  needed), and the ESP32 connects there. DIO2/DIO3 stay with the USB-serial chip.
- To reflash, go to the board: plug in micro-USB, hold BTN1 through a reset
  (with `BL_PIN` set to its DIO), and run
  `cc2538-bsl.py -p /dev/tty.usbserial-XXXX -e -w -v coordinator.hex`. The
  XDS110 on the JTAG header works too.
- GPIO5 and the backdoor wire are not needed. GPIO4 → nRST stays, so the ESP
  can still reset a hung CC1312.

The resistance check below decides which wiring applies.

### Remote reflash sequence

1. GPIO5 low (hold the backdoor pin).
2. GPIO4 low for about 10 ms, then release it (reset).
3. Wait about 100 ms, then release GPIO5. The ROM bootloader is now running on
   DIO2/DIO3.
4. The ESP sends the new image with the ROM bootloader protocol (the one
   `cc2538-bsl` speaks), taking the image from an OTA download.
5. Pulse GPIO4 again to boot the new image.

## Still to find out on the board

- Which DIO BTN1 uses (continuity check), and that it has a pull-up.
- The resistance between the USB-serial chip's TX pin and DIO2 (≥ about 1 kΩ:
  share DIO2; less: reflash by hand).
- That the D0–D15 header numbering above matches (continuity check).

No schematic or product page for "CC1310 DK V1.3" turned up online, so the
board facts above come from its silkscreen.

[SWRA466]: https://www.ti.com/document-viewer/lit/html/SWRA466E/GUID-D5C28001-7ADF-45AD-ADAC-783FABF4D8A2
[SWRZ078]: https://www.ti.com/lit/er/swrz078/swrz078.pdf
