# reference/ — specs & oracles (not shipped)

- **freedo** (`CappyT/freedo`): ESP32+CC1101 reimplementation. Its `tado/` Go
  package is the behavioural spec and golden-fixture source. Confirm its licence
  before reusing any code; otherwise clean-room only. Suggested: add as a git
  submodule here.
- **re-lab** (sibling repo): the RE lab — solved PHY, Python decoders,
  `tools/crack_pairing_key.py` (CCM* + pairing-key recovery), and the captures
  used as test fixtures.
