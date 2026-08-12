#!/usr/bin/env python3
"""Write the ZG23 bootloader key tokens over the firmware console.

The firmware holds no key values. This script reads them from a key file you
supply, converts them to the flash layout, and writes them with the `wr`
command. `wr` refuses unless the target range already reads 0xFF, so run this on
a bricked board whose tokens are blank, or erase page 63 first.

Key file format, hex, one per line (do not commit it):

    sign_x: <64 hex chars>   # MFG_SIGNED_BOOTLOADER_KEY_X, 32 bytes
    sign_y: <64 hex chars>   # MFG_SIGNED_BOOTLOADER_KEY_Y, 32 bytes
    enc:    <32 hex chars>   # MFG_SECURE_BOOTLOADER_KEY, 16 bytes

sign_x and sign_y are the coordinate byte strings as they sit in flash. enc is
the 16-byte encryption key. The script pads the enc span with 0xFF to its word
boundary at 0x0807E284.

Usage: write_tokens.py <port> <keys-file>
"""

import re
import sys
import time

import serial

SIGN_SPAN_ADDR = 0x0807E34C  # X || Y, 64 bytes
ENC_SPAN_ADDR = 0x0807E284   # 0xFF pad, enc key at +2, 0xFF pad; 20 bytes


def read_until_prompt(p, timeout=8.0):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        buf += p.read(512)
        if buf.rstrip().endswith(b"swd>"):
            break
    return buf.decode("utf-8", "replace")


def words_le(data):
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


def load_keys(path):
    vals = {}
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"(\w+)\s*[:=]\s*([0-9A-Fa-f]+)", line)
        if m:
            vals[m.group(1).lower()] = bytes.fromhex(m.group(2))
    for name, size in (("sign_x", 32), ("sign_y", 32), ("enc", 16)):
        if name not in vals:
            sys.exit(f"key file is missing '{name}'")
        if len(vals[name]) != size:
            sys.exit(f"'{name}' must be {size} bytes, got {len(vals[name])}")
    return vals


def wr(p, addr, words):
    cmd = "wr 0x%08X %s" % (addr, " ".join("0x%08X" % w for w in words))
    p.reset_input_buffer()
    p.write((cmd + "\r\n").encode())
    p.flush()
    return read_until_prompt(p)


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: write_tokens.py <port> <keys-file>")
    port, keyfile = sys.argv[1:3]
    k = load_keys(keyfile)
    sign_words = words_le(k["sign_x"] + k["sign_y"])           # 16 words at 0x0807E34C
    enc_words = words_le(b"\xff\xff" + k["enc"] + b"\xff\xff")  # 5 words at 0x0807E284

    p = serial.Serial(port, 115200, timeout=0.2)
    p.dtr = True
    p.rts = False
    time.sleep(0.4)
    p.write(b"\r\n")
    p.flush()
    time.sleep(0.2)
    p.reset_input_buffer()

    ok = True
    for name, addr, words in (("sign X||Y", SIGN_SPAN_ADDR, sign_words),
                              ("enc", ENC_SPAN_ADDR, enc_words)):
        resp = wr(p, addr, words)
        status = next((l.strip() for l in resp.splitlines() if l.strip().startswith("wr ")),
                      resp.strip()[-60:])
        print(f"{name}: {status}")
        if ": ok" not in status:
            ok = False
    p.close()

    if not ok:
        print("FAILED - the target range was not blank. Erase page 63 first, or the "
              "board is not bricked.")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
