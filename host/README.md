# host/ — Java control daemon (containerised)

The brain: no hard deadlines. CoAP (Eclipse Californium), 6LoWPAN frag/reasm, MAC
state, CCM* for non-time-critical paths, the tado resource model, coordinator
logic (pair window, network-key generation, device DB, zones/schedules,
persistence), the framed reconnect-tolerant link to the CC1312, and an HTTP API
for Home Assistant.

Build/runtime (Gradle vs Maven, JDK, base image) is an open decision — see
`../docs/PLAN.md` section 8.
