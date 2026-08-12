#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "swd.h"

// Bootloader key tokens in flash page 63 (the lockbits page) of an
// EFR32ZG23A020F512GM40. Addresses from btl_security_tokens.h.
#define TOK_ENC_ADDR 0x0807E286u  // MFG_SECURE_BOOTLOADER_KEY, 16 bytes
#define TOK_X_ADDR   0x0807E34Cu  // MFG_SIGNED_BOOTLOADER_KEY_X, 32 bytes
#define TOK_Y_ADDR   0x0807E36Cu  // MFG_SIGNED_BOOTLOADER_KEY_Y, 32 bytes

// Word-aligned write spans. X and Y are contiguous, so they form one 64-byte
// burst. The encryption key is halfword-aligned, so its burst widens to
// 0x0807E284 and pads with 0xFF, which clears no bits in the neighbours.
#define SIGN_SPAN_ADDR 0x0807E34Cu
#define SIGN_SPAN_WORDS 16
#define ENC_SPAN_ADDR  0x0807E284u
#define ENC_SPAN_WORDS 5

typedef struct {
    uint32_t sign[SIGN_SPAN_WORDS];  // X || Y as read from 0x0807E34C
    uint32_t enc[ENC_SPAN_WORDS];    // enc span as read from 0x0807E284
} token_read_t;

// Read the sign and enc spans over SWD. Sets all_blank when every byte is 0xFF.
swd_result_t zg23_read_tokens(token_read_t *out, bool *all_blank);

#define LOCKBITS_PAGE_ADDR 0x0807E000u  // page 63, holds the tokens plus DSK/QR

typedef void (*zg23_log_fn)(const char *line);

// Erase one flash page via MSC. page_addr must be page-aligned. Erasing the
// lockbits page also blanks the DSK and QR data, so it is not part of recovery.
swd_result_t zg23_erase_page(uint32_t page_addr, zg23_log_fn log_line);

// Write the Gecko bootloader reset cause to RAM and reset-to-run, so the target
// comes up in the bootloader's communication mode, as an OTW update does.
swd_result_t zg23_enter_bootloader(zg23_log_fn log_line);

// Write a contiguous word span via one MSC burst, then read it back and check.
// Refuses unless the target range already reads 0xFF, since flash only clears
// bits. The host supplies the words. n must be 1..64.
swd_result_t zg23_write_span(uint32_t addr, const uint32_t *words, int n,
                             zg23_log_fn log_line);
