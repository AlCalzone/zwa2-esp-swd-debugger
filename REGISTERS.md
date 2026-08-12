# ZG23 register reference

The registers this tool drives on an EFR32ZG23A020F512GM40. Addresses come from
the `simplicity_sdk_2024.12.2` headers. This is the register-level backing for the
`read`, `write`, `erase`, and `wr` commands.

## Token locations

The three key tokens sit in flash page 63, the lockbits page:

| Token | Address | Size | Alignment |
|---|---|---|---|
| `MFG_SIGNED_BOOTLOADER_KEY_X` | `0x0807E34C` | 32 bytes | word |
| `MFG_SIGNED_BOOTLOADER_KEY_Y` | `0x0807E36C` | 32 bytes | word |
| `MFG_SECURE_BOOTLOADER_KEY` | `0x0807E286` | 16 bytes | halfword |

`0x0807E000` is page 63, from `FLASH_BASE + FLASH_SIZE - FLASH_PAGE_SIZE`. The
offsets `0x34C` and `0x36C` are `PUBKEY_OFFSET_X` / `PUBKEY_OFFSET_Y` in
`btl_security_tokens.h`. The `0x286` encryption-key offset is inline in
`btl_getImageFileEncryptionKeyPtr()`.

X and Y are contiguous, so they write as one 64-byte burst. The encryption key is
halfword-aligned, so its burst widens to `0x0807E284` for 20 bytes and pads with
`0xFF` at both ends. `0xFF` clears no bits, so the padding leaves its neighbours
alone.

## MSC and CMU registers

The MSC and CMU are secure peripherals. A debugger reaches them at the secure
alias `0x5003_xxxx` / `0x5000_8xxx`, `0x1000_0000` above the non-secure alias
below. The non-secure alias reads as zero and drops writes. The firmware probes
both and uses the one that responds.

MSC, non-secure base `0x40030000` (reach it at `0x50030000` over SWD):

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

- **`LOCK`** unlock key `0x00001B71`. Writing `0` re-locks.
- **`WRITECTRL.WREN`** bit 0.
- **`WRITECMD.ERASEPAGE`** bit 1, **`WRITECMD.WRITEEND`** bit 2.
- **`STATUS`** bits: `BUSY` 0, `LOCKED` 1, `INVADDR` 2, `WDATAREADY` 3,
  `PENDING` 5, `TIMEOUT` 6, `REGLOCK` 16, `WREADY` 27.
- **`PAGELOCK1`** bit 31 covers page 63. It is set-only and clears on reset, so a
  set bit forces a reset-and-halt before the write.

CMU, non-secure base `0x40008000` (reach it at `0x50008000` over SWD):

- **`CLKEN1`** at `0x40008068`. The MSC clock is bit 16.

## Core debug registers

Standard ARMv8-M addresses:

- **`DHCSR`** `0xE000EDF0`. Halt with `0xA05F0003` (`DBGKEY | C_DEBUGEN | C_HALT`).
  `S_HALT` is bit 17.
- **`DEMCR`** `0xE000EDFC`. `VC_CORERESET` is bit 0 for halt-on-reset.
- **`AIRCR`** `0xE000ED0C`. `0x05FA0004` requests `SYSRESETREQ`.

Reset-and-halt goes through `DEMCR` and `AIRCR`, so no physical reset line is
needed.

## Write sequence

1. Bring up SWD: line reset, JTAG-to-SWD switch, read DPIDR.
2. Power the debug domain through DP `CTRL/STAT` with `CDBGPWRUPREQ` and
   `CSYSPWRUPREQ`. Wait for the ack bits.
3. Select the AHB-AP and set `CSW` to 32-bit accesses.
4. Halt the core with `0xA05F0003` to `DHCSR`.
5. Read the target range. Write only if every byte is `0xFF`.
6. Detect the MSC alias, then enable the MSC clock with bit 16 in `CLKEN1`.
7. Unlock the MSC with `0x00001B71` to `LOCK`. Confirm `STATUS.REGLOCK` is clear.
8. Check `PAGELOCK1` bit 31. If set, reset-and-halt, then re-run from step 6.
9. Set `WRITECTRL.WREN`.
10. Write each span, following `writeBurst` in `em_msc.c`:
    - write the span start address to `ADDRB`.
    - check `STATUS.INVADDR`.
    - write the first word to `WDATA`.
    - for each later word, wait for `STATUS.WDATAREADY`, then write it.
    - write `WRITECMD.WRITEEND`.
    - wait for `STATUS.BUSY` and `STATUS.PENDING` to clear, then check again.
11. Clear `WRITECTRL.WREN` and write `0` to `LOCK`.
12. Read the range back to confirm the write landed. The host compares it to the
    values it meant to write.

A page erase replaces steps 9–11 with `WRITECTRL.WREN`, `ADDRB` = page base,
`WRITECMD.ERASEPAGE`, then a wait for `BUSY` and `PENDING` to clear.
