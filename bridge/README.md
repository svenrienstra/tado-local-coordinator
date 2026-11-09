# bridge/ — ESP32 UART<->WiFi bridge

A transparent UART<->TCP tunnel (460800 baud, auto-reconnect, TLS). No protocol
logic — it moves bytes between the CC1312 and the Java daemon. The CC1312<->brain
framing is end-to-end, so this stays dumb and tolerant of drops.

Options: ESPHome stream-server, a small ESP-IDF sketch, or a tiny Linux box
(`ser2net`) at the radio location (which could also host the container).
