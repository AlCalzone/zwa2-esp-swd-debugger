#!/usr/bin/env python3
"""Put a ZWA-2's ESP32-S3 into ROM download mode and print the port it
re-enumerates on.

The stock firmware watches its USB CDC for a baud-rate knock (150, 300, 600),
drops to a "cmd>" menu, and enters the ROM bootloader on the "BE" command.
Mirrors home-assistant/zwa2-toolbox src/lib/esp-utils.ts.

The download device shares its USB id (303a:1001) with any other generic
Espressif device on the bus, so the new port is found by diffing
/dev/serial/by-id before and after, never by matching that id.

Usage: zwa2_bootloader.py /dev/ttyACMx
"""

import glob
import os
import sys
import time

import serial

MAGIC_BAUDRATES = [150, 300, 600]


def by_id_map():
    return {os.path.realpath(p): p for p in glob.glob("/dev/serial/by-id/*")}


def enter_bootloader(port):
    ser = serial.Serial(port, 115200, timeout=0.2)
    for i, baud in enumerate(MAGIC_BAUDRATES):
        if i:
            time.sleep(0.1)
        ser.close()
        ser.baudrate = baud
        ser.open()

    buf = b""
    deadline = time.time() + 2.0
    while time.time() < deadline:
        buf += ser.read(64)
        if b"cmd>" in buf:
            break
    saw_menu = b"cmd>" in buf

    info = b""
    if saw_menu:
        ser.write(b"I")             # firmware info, echoed back before the menu
        time.sleep(0.4)
        info = ser.read(256)

    try:
        ser.write(b"BE")            # enter ROM bootloader; the CDC drops here
        ser.flush()
    except serial.SerialException:
        pass
    time.sleep(0.3)
    try:
        ser.close()
    except Exception:
        pass
    return saw_menu, buf, info


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: zwa2_bootloader.py /dev/ttyACMx")
    port = sys.argv[1]
    before = set(by_id_map())

    saw_menu, banner, info = enter_bootloader(port)
    sys.stderr.write(f"menu prompt seen: {saw_menu}\n")
    if info:
        sys.stderr.write(f"firmware info: {info!r}\n")

    deadline = time.time() + 6.0
    new = {}
    while time.time() < deadline:
        cur = by_id_map()
        added = {tty: link for tty, link in cur.items() if tty not in before}
        if added:
            new = added
            break
        time.sleep(0.2)

    if not new:
        sys.exit("no new download port appeared; the board did not enter the bootloader")
    for tty, link in sorted(new.items()):
        sys.stderr.write(f"download device: {os.path.basename(link)}\n")
        print(tty)


if __name__ == "__main__":
    main()
