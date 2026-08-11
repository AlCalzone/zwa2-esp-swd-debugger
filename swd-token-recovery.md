# Restoring the bootloader key tokens over SWD

A ZWA-2 whose lockbits page was erased holds blank GBL signing and encryption
tokens. An OTW firmware update then aborts with `file error 0x44`, because the
GBL decrypts with the blank key into something the parser cannot read. No image
can be delivered over UART in that state, so the tokens have to be restored
through the debug interface. This is the register-level sequence for doing that,
as bit-banged SWD on the on-board ESP32-S3.

On an affected board, `commander security status` reports
`Sign key installed : False`. That matters: with no boot key in the SE,
`btl_getSignedBootloaderKeyXPtr()` returns the lockbits pointer rather than
`NULL`, so signature verification really does read the blank token and both
keys need restoring.

All addresses and bit positions below come from the `simplicity_sdk_2024.12.2`
headers for `EFR32ZG23A020F512GM40`, and the source files named throughout —
`btl_security_tokens.h`, `em_msc.c`, `btl_comm_xmodem_common.c` — are the copies
vendored in the `nc-zwave-bootloader` repo.

## Before anything else: confirm the byte order

The key values here were derived from `keys/vendor_sign.key` and
`keys/vendor_encrypt.key` in the `nc-zwave-bootloader` repo, treating the public
key as big-endian SEC1 coordinates. **That the tokens are stored in flash in
this byte order is unverified.** Read the same region from a board with intact
keys and compare before writing to anything. The read path of the tool itself
does this without a J-Link, since steps 1 to 5 below touch nothing; with a
J-Link to hand it is:

```bash
commander readmem --range 0x0807E34C:+64 --device EFR32ZG23A020F512GM40
```

The bytes must come back as X followed by Y:

```
A390F15DC683C68136232CBC09BC5F9C6AACA7020EB20D8C517EC4F18A48A8A0
FC55AB59A900B5199BE7BAFB1B77BCC88686F15FFDF3FEACDEEE5978C1B6117D
```

If they do not, every word table below is wrong. Getting this wrong is not
recoverable by retrying: flash only clears bits, so a token written with
swapped byte order can only be corrected by erasing the whole lockbits page,
which also carries the DSK and QR code data.

## Board wiring

The board carries a populated SWD header with GND, 3.3V, SWDIO, SWCLK and RST,
and the ESP32-S3 brings out UART0 TX/RX plus GPIO0, 5, 6, 7, 8 and 9. Both sit
on the same PCB, so GND and 3.3V are already common — jumper only the two
signal lines:

| ESP32-S3 | SWD header | Direction |
|---|---|---|
| GPIO5 | SWCLK | output |
| GPIO6 | SWDIO | bidirectional |

Identify which header pin is which from the schematic or silkscreen before
wiring. Leave the header's 3.3V unconnected: bridging it to the S3 rail gains
nothing and risks a fight if that pin is switched or separately regulated.

RST is not needed. Reset-and-halt goes through `DEMCR` and `AIRCR` over SWD, so
two wires are enough. If you do want hardware reset, GPIO7 is free.

Two constraints on the pin choice:

- GPIO0 is a strapping pin. Held low at reset it puts the S3 into download
  mode, so it must not carry SWCLK, which idles low.
- GPIO6 through GPIO9 are ordinary GPIOs on the S3, where the SPI flash sits on
  the high pins. On a classic ESP32 those four are the flash pins and this
  assignment would not work.

SWDIO is bidirectional and the ZG23 debug pins have internal pull-ups, so drive
it as open-drain or switch the pin direction per phase rather than driving it
high against the target.

## What gets written

| Token | Address | Size | Alignment |
|---|---|---|---|
| `MFG_SIGNED_BOOTLOADER_KEY_X` | `0x0807E34C` | 32 bytes | word aligned |
| `MFG_SIGNED_BOOTLOADER_KEY_Y` | `0x0807E36C` | 32 bytes | word aligned |
| `MFG_SECURE_BOOTLOADER_KEY` | `0x0807E286` | 16 bytes | halfword aligned |

`0x0807E000` is the last page of main flash, page 63, derived from
`FLASH_BASE + FLASH_SIZE - FLASH_PAGE_SIZE` with the offsets
`PUBKEY_OFFSET_X` (`0x34C`) and `PUBKEY_OFFSET_Y` (`0x36C`) from
`btl_security_tokens.h`. The encryption key offset `0x286` is inline in
`btl_getImageFileEncryptionKeyPtr()` with no macro.

X and Y are contiguous, so they form one 64-byte write. The encryption key is
not word aligned, so its write widens to `0x0807E284` for 20 bytes and pads
with `0xFF` at both ends. `0xFF` padding clears no bits, so it cannot damage
whatever sits in the two bytes on either side.

## Refuse to write unless the tokens are blank

Read all three regions first. Proceed only if every byte is `0xFF`. Any other
content means either the keys are already correct, in which case there is
nothing to do, or they hold something else, which this procedure cannot fix.
Writing over a populated token corrupts a working board.

A user-facing tool must enforce this gate before touching MSC, not merely
document it.

## Register map

**Correction, verified on hardware.** The addresses below are the non-secure
peripheral aliases. A running ZWA-2 configures the MSC and CMU as secure. A
debugger then reaches them only at the secure alias, `0x1000_0000` higher. That
puts the MSC at `0x50030000` and the CMU at `0x50008000`. The non-secure alias
reads as zero and drops writes. A flash write there silently does nothing. The
firmware detects the live alias at run time.

MSC, base `0x40030000` (non-secure alias; reach it at `0x50030000` over SWD):

| Register | Address |
|---|---|
| `WRITECTRL` | `0x4003000C` |
| `WRITECMD` | `0x40030010` |
| `ADDRB` | `0x40030014` |
| `WDATA` | `0x40030018` |
| `STATUS` | `0x4003001C` |
| `LOCK` | `0x4003003C` |
| `PAGELOCK1` | `0x40030124` |

Values and bits:

- `LOCK` unlock key: `0x00001B71`, writing `0` re-locks
- `WRITECTRL.WREN`: bit 0
- `WRITECMD.WRITEEND`: bit 2
- `STATUS`: `BUSY` bit 0, `LOCKED` bit 1, `INVADDR` bit 2, `WDATAREADY` bit 3,
  `PENDING` bit 5, `TIMEOUT` bit 6, `REGLOCK` bit 16, `WREADY` bit 27
- `PAGELOCK1` bit 31 covers page 63, the lockbits page. The bit is set-only and
  clears on reset, so if it is set the device must be reset and halted before
  the write.

CMU, base `0x40008000`:

- `CLKEN1` at `0x40008068`, `MSC` enable is bit 16

Core debug, standard ARMv8-M addresses:

- `DHCSR` `0xE000EDF0`, halt with `0xA05F0003` (`DBGKEY | C_DEBUGEN | C_HALT`)
- `DEMCR` `0xE000EDFC`, `VC_CORERESET` bit 0 for halt-on-reset
- `AIRCR` `0xE000ED0C`, `0x05FA0004` for `SYSRESETREQ`

Reset-and-halt through `DEMCR` and `AIRCR` needs no physical reset line, so the
ESP32-S3 implementation only needs SWCLK and SWDIO. Ground and 3.3V are already
common with the SWD header on the same board — do not jumper the header's 3.3V.

## Sequence

1. Bring up SWD: line reset, JTAG-to-SWD switch sequence, read `IDCODE`.
2. Power the debug domain through DP `CTRL/STAT` with `CDBGPWRUPREQ` and
   `CSYSPWRUPREQ`, then wait for the matching ack bits.
3. Select the AHB-AP and set `CSW` to 32-bit accesses.
4. Halt the core: write `0xA05F0003` to `DHCSR`. Halting keeps the bootloader
   from running while flash is being written.
5. Read the three token regions. Abort unless all bytes are `0xFF`.
6. Enable the MSC bus clock: set bit 16 in `CLKEN1`.
7. Unlock MSC: write `0x00001B71` to `LOCK`. Confirm `STATUS.REGLOCK` is clear.
8. Check `PAGELOCK1` bit 31. If set, reset and halt, then start again from
   step 4.
9. Set `WRITECTRL.WREN`.
10. Write each span, following `writeBurst` in `em_msc.c`:
    - write the span's start address to `ADDRB`
    - check `STATUS.INVADDR`, abort if set
    - write the first word to `WDATA`
    - for each remaining word, wait for `STATUS.WDATAREADY`, then write it
    - write `WRITECMD.WRITEEND`
    - wait for `STATUS.BUSY` and `STATUS.PENDING` to both clear, then check a
      second time, as `em_msc.c` does
11. Clear `WRITECTRL.WREN` and write `0` to `LOCK`.
12. Read all three regions back and compare against the expected values. Report
    failure loudly if they differ — a silently failed restore is what produced
    the blank tokens in the first place.

## Word values

Little-endian words, in write order.

Sign key span, `0x0807E34C`, 64 bytes, 16 words:

```
0x0807E34C  0x5DF190A3
0x0807E350  0x81C683C6
0x0807E354  0xBC2C2336
0x0807E358  0x9C5FBC09
0x0807E35C  0x02A7AC6A
0x0807E360  0x8C0DB20E
0x0807E364  0xF1C47E51
0x0807E368  0xA0A8488A
0x0807E36C  0x59AB55FC
0x0807E370  0x19B500A9
0x0807E374  0xFBBAE79B
0x0807E378  0xC8BC771B
0x0807E37C  0x5FF18686
0x0807E380  0xACFEF3FD
0x0807E384  0x7859EEDE
0x0807E388  0x7D11B6C1
```

Encryption key span, `0x0807E284`, 20 bytes, 5 words. The first and last words
carry `0xFF` padding:

```
0x0807E284  0x8F7FFFFF
0x0807E288  0xB37939E5
0x0807E28C  0xFC56C51B
0x0807E290  0xF4AF31B1
0x0807E294  0xFFFF1424
```

## Validating the restore

A readback only shows the bytes landed. The proof that the bootloader accepts
them is an OTW firmware update: re-flash controller firmware 1.2.0 through
zwave-js's OTW procedure. It is successful if it does not abort with
`file error 0x44`.

The bootloader prints these codes on the xmodem console. The mapping is the
switch at the end of the transfer case in `btl_comm_xmodem_common.c`:

| Printed | Error | Meaning |
|---|---|---|
| `0x43` | `PARSER_CRC` | GBL CRC mismatch |
| `0x44` | `PARSER_UNKNOWN_TAG` | Decrypted stream is unparseable |
| `0x45` | `PARSER_SIGNATURE` | Decryption worked, signature did not verify |
| `0x50` | `PARSER_KEYERROR` | Key error |

`0x44` is the failure a blank `MFG_SECURE_BOOTLOADER_KEY` produces: the GBL is
decrypted with the wrong key, so the parser hits a tag that means nothing. It is
the code to get rid of, and it is reached before signature verification.

That makes the codes a ladder rather than a pass/fail. If the update gets past
`0x44` and stops at `0x45`, the encryption token is now correct and the signing
token is not — progress, not a regression. Both tokens right means the update
runs to completion.
