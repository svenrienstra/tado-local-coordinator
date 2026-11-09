# bridge/ — ESP32 UART<->WiFi bridge

A transparent UART<->TCP tunnel (460800 baud, auto-reconnect). No protocol
logic — it moves bytes between the CC1312 and the Java daemon. The CC1312<->brain
framing is end-to-end and AES-CCM-encrypted, so this stays dumb, needs no TLS,
and tolerates drops.

Also wires GPIOs to the CC1312 `RESET_N` and bootloader backdoor pin so the
CC1312 can be reflashed over the network.

Options: ESPHome stream-server, a small ESP-IDF sketch, or a tiny Linux box
(`ser2net`) at the radio location (which could also host the container).
