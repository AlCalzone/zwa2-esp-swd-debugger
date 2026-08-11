#!/usr/bin/env python3
"""Read the ZG23 bootloader key tokens over SWD and compare them to the values
the recovery plan expects.

The expected bytes are derived from the little-endian word tables in
swd-token-recovery.md and cross-checked against the big-endian X/Y coordinate
strings quoted at the top of that plan. This is the "confirm the byte order"
step: run it against a board with intact keys before trusting any write path.

Two read backends:
  --backend jlink   drive a SEGGER J-Link with JLinkExe (default)
  --input FILE      parse a saved JLinkExe log instead of connecting

The three token regions live in flash page 63 (the lockbits page):
  MFG_SECURE_BOOTLOADER_KEY    0x0807E286  16 bytes
  MFG_SIGNED_BOOTLOADER_KEY_X  0x0807E34C  32 bytes
  MFG_SIGNED_BOOTLOADER_KEY_Y  0x0807E36C  32 bytes
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

DEVICE = "EFR32ZG23A020F512GM40"

# --- Expected token bytes, derived from the plan's word tables --------------

_SIGN_WORDS = [
    0x5DF190A3, 0x81C683C6, 0xBC2C2336, 0x9C5FBC09,
    0x02A7AC6A, 0x8C0DB20E, 0xF1C47E51, 0xA0A8488A,
    0x59AB55FC, 0x19B500A9, 0xFBBAE79B, 0xC8BC771B,
    0x5FF18686, 0xACFEF3FD, 0x7859EEDE, 0x7D11B6C1,
]
_ENC_WORDS = [0x8F7FFFFF, 0xB37939E5, 0xFC56C51B, 0xF4AF31B1, 0xFFFF1424]


def _flash_bytes(words):
    b = bytearray()
    for w in words:
        b += w.to_bytes(4, "little")
    return bytes(b)


_SIGN = _flash_bytes(_SIGN_WORDS)          # 64 bytes at 0x0807E34C: X || Y
_ENC_SPAN = _flash_bytes(_ENC_WORDS)       # 20 bytes at 0x0807E284, FF-padded

EXPECTED = {
    "MFG_SECURE_BOOTLOADER_KEY (enc)": (0x0807E286, _ENC_SPAN[2:18]),
    "MFG_SIGNED_BOOTLOADER_KEY_X":     (0x0807E34C, _SIGN[:32]),
    "MFG_SIGNED_BOOTLOADER_KEY_Y":     (0x0807E36C, _SIGN[32:]),
}

# Self-check: the word tables must reconstruct the plan's big-endian strings.
_X_BE = bytes.fromhex("A390F15DC683C68136232CBC09BC5F9C6AACA7020EB20D8C517EC4F18A48A8A0")
_Y_BE = bytes.fromhex("FC55AB59A900B5199BE7BAFB1B77BCC88686F15FFDF3FEACDEEE5978C1B6117D")
assert _SIGN[:32] == _X_BE, "X word table does not match quoted big-endian X"
assert _SIGN[32:] == _Y_BE, "Y word table does not match quoted big-endian Y"
assert _ENC_SPAN[0:2] == b"\xff\xff" and _ENC_SPAN[18:20] == b"\xff\xff"

# Whole span actually read back, so a single dump covers all three regions.
READ_BASE = 0x0807E280
READ_LEN = 0x120


def read_via_jlink(jlink_exe, speed):
    """Return {address: byte} read from the live target, or raise on failure."""
    script = (
        "si SWD\n"
        f"speed {speed}\n"
        f"device {DEVICE}\n"
        "connect\n"
        f"mem 0x{READ_BASE:08X}, 0x{READ_LEN:X}\n"
        "exit\n"
    )
    with tempfile.NamedTemporaryFile("w", suffix=".jlink", delete=False) as f:
        f.write(script)
        script_path = f.name
    try:
        out = subprocess.run(
            [jlink_exe, "-NoGui", "1", "-ExitOnError", "1",
             "-CommanderScript", script_path],
            capture_output=True, text=True, timeout=60,
        ).stdout
    finally:
        os.unlink(script_path)

    if "Target voltage too low" in out:
        raise SystemExit(
            "J-Link reports VTref too low: its SWD leads are not on a powered "
            "target. Wire VTref->3V3, SWDIO, SWCLK, GND to the ZWA-2 SWD header "
            "and power the board over USB, then rerun.")
    if "Cannot connect" in out or "Could not connect" in out:
        raise SystemExit("J-Link could not connect to the target:\n" + out)
    return out


def parse_mem_dump(text):
    """Parse JLinkExe 'mem' output lines: '0807E280 = FF FF 7F 8F ...'.

    JLinkExe prints 16 bytes per line, sometimes followed by an ASCII column.
    Take at most 16 two-hex-digit tokens after '=' so the ASCII column, which
    can contain hex-looking characters, never leaks into the byte stream.
    READ_LEN is a multiple of 16, so every line here is full.
    """
    mem = {}
    for line in text.splitlines():
        left, sep, right = line.partition("=")
        if not sep or not re.fullmatch(r"\s*[0-9A-Fa-f]{4,8}\s*", left):
            continue
        base = int(left.strip(), 16)
        for i, tok in enumerate(re.findall(r"\b[0-9A-Fa-f]{2}\b", right)[:16]):
            mem[base + i] = int(tok, 16)
    return mem


def extract(mem, addr, length):
    try:
        return bytes(mem[addr + i] for i in range(length))
    except KeyError:
        raise SystemExit(
            f"dump did not cover 0x{addr:08X}..0x{addr + length:08X}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", choices=["jlink"], default="jlink")
    ap.add_argument("--input", help="parse a saved JLinkExe log instead of connecting")
    ap.add_argument("--jlink-exe", default="JLinkExe")
    ap.add_argument("--speed", type=int, default=4000)
    args = ap.parse_args()

    if args.input:
        with open(args.input) as f:
            text = f.read()
    else:
        text = read_via_jlink(args.jlink_exe, args.speed)

    mem = parse_mem_dump(text)
    if not mem:
        raise SystemExit("no memory bytes parsed from the dump:\n" + text)

    all_ff = True
    ok = True
    print(f"{'region':32} {'addr':>10}  result")
    print("-" * 72)
    for name, (addr, expected) in EXPECTED.items():
        got = extract(mem, addr, len(expected))
        blank = all(b == 0xFF for b in got)
        all_ff = all_ff and blank
        match = got == expected
        ok = ok and match
        status = "MATCH" if match else ("BLANK(0xFF)" if blank else "DIFFERS")
        print(f"{name:32} 0x{addr:08X}  {status}")
        print(f"    expected {expected.hex()}")
        print(f"    read     {got.hex()}")

    print("-" * 72)
    if all_ff:
        print("All three regions are blank (0xFF). This is a bricked board, not a "
              "reference. Read a board with intact keys to verify byte order.")
        return 2
    if ok:
        print("VERIFIED: on-silicon byte order matches the plan. The write path "
              "word tables are safe to use.")
        return 0
    print("MISMATCH: the board stores the tokens in a different order than the "
          "plan assumes. Do NOT write with the current word tables.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
