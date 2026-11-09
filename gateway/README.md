# gateway/ — ESP32 gateway (ESP-IDF, C)

Not started; begins at M1/M2 (`../docs/PLAN.md` 4.2).

The brain of the deployed coordinator. It talks to the CC1312 over UART with
the host link (`../docs/HOSTLINK.md`) and runs 6LoWPAN, CoAP and the tado
resource model: pairing, `auth/token`, `time` (from SNTP), device reports and
setpoint writes. It keeps the network key, frame-counter floor and device table
in NVS, publishes device state over MQTT with Home Assistant discovery, and
takes commands such as setpoints and the pair window from MQTT
(`../docs/PLAN.md` 3.7). Updates over WiFi (OTA).

It shares the portable C core of `../firmware/src` (CRC, CCM*, MAC frames).

Wiring to the CC1312 board: `../docs/WIRING.md`.
