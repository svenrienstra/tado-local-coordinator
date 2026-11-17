# tools/sniff — timing from on-air captures

The CC1312 timestamps every frame it receives with its radio timer (RAT). With a
`make SNIFF=1` build it reports every frame it cannot open, other coordinators'
Enhanced ACKs included. That is how the train and ACK timings in
`../../firmware/README.md` ("On air") were measured against the commercial
bridge.

1. Flash a sniff build: `cd firmware && make clean flash SNIFF=1`. Keep the
   commercial bridge on, so its traffic is on air.
2. Capture: from `host/`, `java -cp target/tado-coordinator-host.jar
   tadocoord.bench.SniffBench <port> capture.txt 180`.
3. Analyse with Python 3 (no packages needed):
   - `analyze_sniff.py capture.txt`: per wake-up train the frame step, the
     rendezvous values, when the Data frame starts against what the train
     announced, and the Enhanced ACK gaps.
   - `check_anchor.py capture.txt <bridge EUI>`: each device's channel sample
     from its Enhanced ACKs (end minus phase x 320 us), whether it stays stable,
     and where the bridge's short trains sit against it.
4. Flash the normal build again: `make clean flash`.

Frame starts are computed from the frame end assuming our 4-byte preamble, so
absolute starts can be off by the difference in the sender's preamble; gaps
between frames of the same sender are not.
