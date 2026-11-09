# Wiring

Two setups. Bench mode (M-1 to M2) needs no extra wiring; bridge mode (M3.5
onward) adds an ESP32 next to the radio board.

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

| CC1312 pin | Use | Set in |
|---|---|---|
| `UART_RX_DIO` (default DIO2) | UART0 RX (host → CC1312) | `firmware/Makefile` |
| `UART_TX_DIO` (default DIO3) | UART0 TX (CC1312 → host) | `firmware/Makefile` |
| RESET_N (nRST) | reset, active low | — |
| `BL_PIN` (default: none) | bootloader backdoor, active low | `firmware/Makefile` |

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

## Bridge mode — ESP32 over WiFi

The USB-serial chip stays wired to DIO2/DIO3. If the ESP32 shared those lines,
both would drive DIO2 and the bytes would get corrupted. Instead, the bridge
build moves UART0 to two free DIOs (the CC1312 can route its UART to any DIO):

```
make clean all UART_RX_DIO=<rx> UART_TX_DIO=<tx> BL_PIN=<backdoor>
```

```
        ESP32 (DevKitC)                         CC1312R1 board
   ┌──────────────────────┐                ┌──────────────────────────┐
   │ GPIO17  (UART2 TX) ──┼──────────────► │ D<rx>  UART0 RX          │
   │ GPIO16  (UART2 RX) ◄─┼─────────────── │ D<tx>  UART0 TX          │
   │ GPIO25  open-drain ──┼──────────────► │ nRST   (lower header)    │
   │ GPIO26  open-drain ──┼──────────────► │ D<backdoor>              │
   │ GND ─────────────────┼─────────────── │ GND (JTAG header or VBAT−)│
   └──────────┬───────────┘                └────────────┬─────────────┘
              │ USB 5 V                                 │ micro-USB 5 V
          [ power ]                                 [ power ]
```

- **Cross the UART lines:** ESP TX goes to CC1312 RX, and ESP RX comes from
  CC1312 TX. The UART runs at 460800 8N1.
- **Choose free DIOs:** pick pins that nothing else on the board uses. The
  buttons and the LCD connector take some DIOs; the LCD traces appear to run
  towards the lower header. Probe for continuity before choosing.
- **Choose the backdoor pin carefully:** it must read high at reset. BTN1 is the
  natural choice if it pulls a DIO to ground with a pull-up, as on TI's
  LaunchPad (DIO13). Find its DIO first. The backdoor is off unless the build
  sets `BL_PIN`, so a wrong guess cannot leave the board stuck in the bootloader.
- **Drive GPIO25 and GPIO26 as open-drain** (pull low or release, never drive
  high). The RESET button and BTN1 also pull those nets to ground, and while the
  ESP boots its pins float, which reads as released.
- **Share ground.** Both sides use 3.3 V logic, so no level shifter is needed.
  Keep the board on its micro-USB supply. That also keeps the USB-serial port
  usable for debugging on site.
- GPIO16/17 are free on an ESP32-WROOM DevKitC. On a WROVER module they belong
  to the PSRAM, so pick two other GPIOs there.

### Remote reflash sequence

1. GPIO26 low (hold the backdoor pin).
2. GPIO25 low for about 10 ms, then release it (reset).
3. Wait about 100 ms, then release GPIO26. The ROM bootloader is now running.
4. Run `cc2538-bsl` against the bridge's TCP port.
5. Pulse GPIO25 again to boot the new image.

One catch: the ROM bootloader listens on its own fixed UART pins, not on the
pins the firmware chose, so a remote reflash through the ESP's UART pins may
not work. Check TI's bootloader documentation for the CC13x2 before relying on
this.

## Still to find out on the board

- Which DIOs BTN1 and BTN2 use, and whether they have pull-ups.
- Which DIOs the LCD connector uses, and which pins are free for the ESP.
- That the D0–D15 header numbering above matches (continuity check).
- Which UART pins the ROM bootloader uses on this chip, and whether
  `cc2538-bsl` works through a TCP socket at 460800.
