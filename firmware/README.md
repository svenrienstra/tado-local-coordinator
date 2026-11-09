# firmware/ — CC1312 MAC engine (C)

Real-time radio tier. Owns everything with a deadline: PHY TX/RX (RF core,
RAT-timed), 802.15.4-2015 MAC, CSL (wake-up trains, timed TX windows,
Enhanced-ACK in the 0-1 ms gap), AES-CCM*, and the local autonomy layer that
keeps the network alive when the brain is unreachable.

- Base: port from `re-lab/firmware/cc1312-minimal-rx` (raw TI driverlib +
  NoRTOS RF driver). TX is new — there is none in the sniffer firmware.
- First work item: **M0** (TX + CSL timing proof). See `../docs/PLAN.md`.
- Safety: keep bootloader/debug unlocked; never enable fuse/read-out locks.
