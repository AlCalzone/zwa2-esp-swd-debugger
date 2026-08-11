#!/usr/bin/env bash
# Flash the SWD token tool onto an ESP32-S3 in download mode.
# Usage: firmware/flash.sh /dev/ttyACMx
set -e
export PATH="$HOME/esp/pyshim:$PATH"
. "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
PORT="${1:?usage: flash.sh /dev/ttyACMx}"
idf.py -C "$(cd "$(dirname "$0")" && pwd)" -p "$PORT" flash
