#!/usr/bin/env python3
"""Restore flash from a backup where the device is blank but the backup is not.

Reads the current page over the `dump` command, and for every word that is
0xFFFFFFFF on the device but populated in the backup, writes it back via `wr` in
contiguous runs. Words that already match (e.g. freshly written tokens) are left
alone. Verifies at the end.

Usage: restore_flash.py <port> <backup.bin> <hex_base>
"""

import re
import sys
import time

import serial

MAX_RUN = 32
LINE = re.compile(r"0x([0-9A-Fa-f]{8})\s+0x([0-9A-Fa-f]{8})")


def read_until_prompt(p, timeout=8.0):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        buf += p.read(512)
        if buf.rstrip().endswith(b"swd>"):
            break
    return buf.decode("utf-8", "replace")


def dump(p, base, nwords):
    mem = {}
    for a in range(base, base + nwords * 4, 64 * 4):
        n = min(64, (base + nwords * 4 - a) // 4)
        p.reset_input_buffer()
        p.write(f"dump 0x{a:08X} {n}\r\n".encode())
        p.flush()
        for m in LINE.finditer(read_until_prompt(p)):
            mem[int(m.group(1), 16)] = int(m.group(2), 16)
    return mem


def wr(p, addr, words):
    cmd = "wr 0x%08X %s" % (addr, " ".join("0x%08X" % w for w in words))
    p.reset_input_buffer()
    p.write((cmd + "\r\n").encode())
    p.flush()
    return read_until_prompt(p)


def main():
    port, backup, base_s = sys.argv[1:4]
    base = int(base_s, 16)
    data = open(backup, "rb").read()
    nwords = len(data) // 4
    bk = [int.from_bytes(data[i * 4:i * 4 + 4], "little") for i in range(nwords)]

    p = serial.Serial(port, 115200, timeout=0.2)
    p.dtr = True
    p.rts = False
    time.sleep(0.4)
    p.write(b"\r\n")
    p.flush()
    time.sleep(0.2)
    p.reset_input_buffer()

    cur = dump(p, base, nwords)
    todo = [(base + i * 4, bk[i]) for i in range(nwords)
            if bk[i] != 0xFFFFFFFF and cur.get(base + i * 4, 0xFFFFFFFF) == 0xFFFFFFFF]
    print(f"{len(todo)} word(s) to restore")

    runs = []
    for a, w in todo:
        if runs and a == runs[-1][0] + 4 * len(runs[-1][1]) and len(runs[-1][1]) < MAX_RUN:
            runs[-1][1].append(w)
        else:
            runs.append([a, [w]])
    for a, words in runs:
        resp = wr(p, a, words).replace("\n", " ").strip()
        print(f"  wr 0x{a:08X} x{len(words)}: ...{resp[-48:]}")

    cur = dump(p, base, nwords)
    bad = [base + i * 4 for i in range(nwords)
           if cur.get(base + i * 4, 0xFFFFFFFF) != bk[i]]
    print("VERIFY:", "page matches backup" if not bad
          else f"{len(bad)} mismatch(es): {[hex(x) for x in bad[:8]]}")
    p.close()
    sys.exit(0 if not bad else 1)


if __name__ == "__main__":
    main()
