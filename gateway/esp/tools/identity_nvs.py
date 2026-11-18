#!/usr/bin/env python3
"""Writes the Java bench tool's identity as an NVS partition CSV for the gateway.

Devices joined with the bench tool hold its network key and home id; the
gateway must start with the same ones, or they have to be paired again. Such a
device shows up on the setup page in no room once it talks to the gateway; put
it in a room there. The output holds the network key: it is created
owner-only, keep it out of git and delete it once flashed.

    tools/identity_nvs.py ../../host/state/coordinator.properties identity.csv
    python -m esp_idf_nvs_partition_gen generate identity.csv identity.bin 0x10000
    esptool.py --chip esp32s3 write_flash 0x9000 identity.bin

The layout matches main/main.c: namespace "tado", blobs eui and key, u32
floor, channel and home. Rooms and devices (blob inst) start empty.
"""

import argparse
import os
import re
import sys

CHANNEL_KHZ = 868335
COUNTER_BLOCK = 4096  # what one bench run reserves; the gateway reserves its own


def read_properties(path):
    props = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                props[k.strip()] = v.strip()
    return props


def hex_bytes(text, n, what):
    if not re.fullmatch(r"[0-9a-fA-F]{%d}" % (2 * n), text):
        sys.exit(f"{what}: expected {n} bytes of hex")
    return bytes.fromhex(text)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("properties", help="host/state/coordinator.properties")
    p.add_argument("csv", help="the NVS CSV to write (holds the network key)")
    a = p.parse_args()

    props = read_properties(a.properties)
    eui = hex_bytes(props["coordinator_eui"], 8, "coordinator_eui")
    key = hex_bytes(props["network_key"], 16, "network_key")
    if eui[5:] != b"\xc5\x1b\x00":
        sys.exit("coordinator_eui must end in c5 1b 00")
    # The bench tool keeps its floor a block ahead already; one more for safety.
    floor = int(props["counter_floor"]) + COUNTER_BLOCK
    channel = int(props.get("channel_khz", CHANNEL_KHZ))
    home = int(props["home_id"])

    rows = [
        "key,type,encoding,value",
        "tado,namespace,,",
        f"eui,data,hex2bin,{eui.hex()}",
        f"key,data,hex2bin,{key.hex()}",
        f"floor,data,u32,{floor}",
        f"channel,data,u32,{channel}",
        f"home,data,u32,{home}",
    ]
    fd = os.open(a.csv, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write("\n".join(rows) + "\n")
    print(f"wrote {a.csv}: home {home}, counter floor {floor}, carrier {channel} kHz")


if __name__ == "__main__":
    main()
