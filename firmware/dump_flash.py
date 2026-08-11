#!/usr/bin/env python3
"""Dump a ZG23 flash range over the firmware's serial console to a binary file.

Reads via the `dump` command in 64-word chunks, syncing on the `swd>` prompt
between chunks so it never races the console. Missing words are filled with
0xFF and reported.

Usage: dump_flash.py <port> <hex_addr> <num_bytes> <out.bin>
"""

import re
import sys
import time

import serial

CHUNK_WORDS = 64
LINE = re.compile(r"0x([0-9A-Fa-f]{8})\s+0x([0-9A-Fa-f]{8})")


def read_until_prompt(p, timeout=4.0):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        buf += p.read(512)
        if buf.rstrip().endswith(b"swd>"):
            break
    return buf.decode("utf-8", "replace")


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: dump_flash.py <port> <hex_addr> <num_bytes> <out.bin>")
    port, addr_s, nbytes_s, out = sys.argv[1:5]
    addr = int(addr_s, 16)
    nbytes = int(nbytes_s, 0)
    if addr % 4 or nbytes % 4:
        sys.exit("addr and length must be word aligned")

    p = serial.Serial(port, 115200, timeout=0.2)
    p.dtr = True
    p.rts = False
    time.sleep(0.2)

    mem = {}
    for base in range(addr, addr + nbytes, CHUNK_WORDS * 4):
        n = min(CHUNK_WORDS, (addr + nbytes - base) // 4)
        p.reset_input_buffer()
        p.write(f"dump 0x{base:08X} {n}\r\n".encode())
        p.flush()
        for m in LINE.finditer(read_until_prompt(p)):
            a = int(m.group(1), 16)
            if addr <= a < addr + nbytes:
                mem[a] = int(m.group(2), 16)
    p.close()

    nwords = nbytes // 4
    missing = [a for a in range(addr, addr + nbytes, 4) if a not in mem]
    data = bytearray()
    for a in range(addr, addr + nbytes, 4):
        data += mem.get(a, 0xFFFFFFFF).to_bytes(4, "little")
    with open(out, "wb") as f:
        f.write(data)

    print(f"read {len(mem)}/{nwords} words -> {out}")
    if missing:
        print(f"MISSING {len(missing)} words, sample: {[hex(x) for x in missing[:8]]}")
    spans = []
    cur = None
    for a in range(addr, addr + nbytes, 4):
        if mem.get(a, 0xFFFFFFFF) != 0xFFFFFFFF:
            cur = [a, a] if cur is None else [cur[0], a]
        elif cur:
            spans.append(cur)
            cur = None
    if cur:
        spans.append(cur)
    print("non-blank spans:")
    for s, e in spans:
        print(f"  0x{s:08X}..0x{e + 3:08X}  ({(e - s) // 4 + 1} words)")


if __name__ == "__main__":
    main()
