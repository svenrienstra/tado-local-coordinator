# reference/ — specs & oracles (not shipped)

- **freedo** (`CappyT/freedo`, Apache-2.0): ESP32+CC1101 reimplementation. Its
  `tado/` Go package and `docs/PROTOCOL.md` are the behavioural spec and
  golden-fixture source. Code may be reused with attribution (keep its NOTICE).
  Git submodule at `reference/freedo` (`git submodule update --init`).
- **Captures**: our own on-air captures of real tado traffic, used as test
  fixtures alongside freedo's `testdata`.
