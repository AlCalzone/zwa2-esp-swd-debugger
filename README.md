# zwa2-esp-swd-debugger

An ESP32-S3 bit-bangs SWD to the EFR32ZG23 next to it on a Nabu Casa ZWA-2. It
reads and writes the ZG23 over the debug port from a serial console. No external
probe is needed. The two signal lines run from the S3's GPIOs to the ZG23 SWD
header. The console is the S3's native USB-Serial-JTAG.

The main job is restoring the ZG23's GBL bootloader key tokens. Blank tokens make
OTW firmware updates abort with `file error 0x44`. The firmware also works as a
general SWD tool. It does MEM-AP memory read/write, core halt and reset, and a
reboot-into-bootloader handoff.

## Wiring

The S3 and ZG23 share GND and 3.3V on the ZWA-2 board. Only two jumpers are
needed, from the S3's broken-out GPIOs to the SWD header:

| ESP32-S3 | ZG23 SWD header | Direction |
|---|---|---|
| GPIO5 | SWCLK | output |
| GPIO6 | SWDIO | bidirectional |

- **3.3V:** leave the header's 3.3V unconnected. It is already common with the S3
  rail.
- **GPIO0:** keep SWCLK off GPIO0. GPIO0 is a strapping pin. SWCLK idles low, and
  on GPIO0 that low level forces download mode at reset.
- **Pin override:** pass `-DSWD_PIN_SWCLK=` / `-DSWD_PIN_SWDIO=` at build time.
- **J-Link:** lift any J-Link's SWDIO and SWCLK leads off the header first. One
  master drives the bus at a time.

## Build

ESP-IDF 5.5, target esp32s3. ESP-IDF installs against Python 3.9 to 3.13. A system
Python 3.14 will not work. Run `install.sh` under a 3.12 or 3.13 interpreter.

```bash
. ~/esp/esp-idf/export.sh
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
```

## Flash

The ZWA-2's S3 has no working DTR/RTS reset over its USB CDC. esptool cannot put
stock firmware into the bootloader on its own.

**First flash, over stock ZWA-2 firmware.** Knock the S3 into the ROM bootloader
with the baud-rate sequence its firmware watches for. Then flash with `--before
no_reset`:

```bash
python3 firmware/zwa2_bootloader.py /dev/ttyACMx   # prints the download port
python -m esptool --chip esp32s3 --port <download-port> --before no_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size keep \
  0x0 firmware/build/bootloader/bootloader.bin \
  0x8000 firmware/build/partition_table/partition-table.bin \
  0x10000 firmware/build/swd_recover.bin
```

An interrupted flash wedges the ROM stub. Recover it by powering up with GPIO0
held low to force a clean download mode.

**Re-flash, once this firmware runs.** It uses USB-Serial-JTAG. esptool resets
that normally:

```bash
firmware/flash.sh /dev/ttyACMx
```

Open the console with any serial terminal, such as `idf.py -C firmware -p
/dev/ttyACMx monitor`. Assert DTR or the console stays silent.

## Console commands

At the `swd>` prompt:

- **`id`** — bring up SWD and print DPIDR and the AP IDR.
- **`read`** — read the three key-token regions and classify them MATCH, BLANK, or
  DIFFERS.
- **`dump <hexaddr> <words>`** — read up to 64 words over the MEM-AP.
- **`halt`** — halt the core.
- **`reset`** — reset the core and halt it at the vector.
- **`run`** — reset the core and let it run, to leave the bootloader for the app.
- **`write confirm`** — restore the key tokens. It refuses unless all three regions
  read `0xFF`.
- **`wr <hexaddr> <word...>`** — write a word span in page 63 through one MSC burst,
  then verify.
- **`erase confirm`** — erase the whole 8 KB lockbits page 63.
- **`bootloader`** — reset the ZG23 into its OTW communication mode.

Details:

- **`write` vs `wr`:** `write` carries the fixed correct token values and is gated
  to a blank page. `wr` writes whatever words you pass. It checks only page-63
  bounds and alignment. It is the raw primitive under `write`.
- **`erase`** blanks the tokens along with the DSK and QR data in the same page.
  Use it to test the write path on an expendable board. Keep it out of recovery.
- **`bootloader`** writes the Gecko reset cause `0xF00F0202` to the first RAM word
  and reset-runs. That is the handoff the app performs before an OTW update. `run`
  sends the ZG23 back to the app.
- Flash-touching commands auto-detect the MSC alias. They probe it at the secure
  alias `0x50030000` and the non-secure alias `0x40030000`. They use whichever
  one responds.

## Host helpers

- **`firmware/zwa2_bootloader.py <port>`** — knock the S3 into ROM download mode.
- **`firmware/dump_flash.py <port> <hexaddr> <bytes> <out.bin>`** — dump a flash
  range to a file over the console.
- **`firmware/restore_flash.py <port> <backup.bin> <hexbase>`** — write a page
  backup into cells that read blank on the device, through `wr`.

## Recovery procedure

1. Flash this firmware onto the target's S3 and add the two jumpers.
2. Run `read`. A bricked board reads BLANK.
3. Run `write confirm`. It writes both spans and reads back VERIFIED.
4. Confirm the fix with an OTW update of controller firmware 1.2.0. Success is an
   update that no longer aborts with `0x44`.

## Safety

`write` reads all three regions first. It proceeds only if every byte is `0xFF`. A
populated token means the keys are already correct. It can also mean page 63 holds
something this procedure cannot fix. Either way `write` leaves the board untouched.
Every flash op reads back and reports a mismatch loudly. `erase` and `wr` have no
such guard. They can destroy manufacturing data. Use them only on boards you can
restore.

## Validation ladder

The bootloader prints an xmodem error code on a failed OTW update:

- **`0x43`** — GBL CRC mismatch.
- **`0x44`** — decrypted stream unparseable. This is the blank-encryption-key
  failure.
- **`0x45`** — decryption worked. The signature did not verify.

`0x44` is what a blank encryption token produces. Reaching `0x45` means the
encryption token is right and the signing token is not. Both tokens right runs to
completion.
