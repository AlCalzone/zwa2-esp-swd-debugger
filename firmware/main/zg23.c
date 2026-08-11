#include "zg23.h"

#include <string.h>
#include <stdio.h>
#include "esp_rom_sys.h"
#include "esp_timer.h"

// Little-endian words in write order, from swd-token-recovery.md. Reading these
// spans back byte-for-byte reproduces the big-endian X, Y and encryption key.
const uint32_t k_sign_span[SIGN_SPAN_WORDS] = {
    0x5DF190A3, 0x81C683C6, 0xBC2C2336, 0x9C5FBC09,
    0x02A7AC6A, 0x8C0DB20E, 0xF1C47E51, 0xA0A8488A,
    0x59AB55FC, 0x19B500A9, 0xFBBAE79B, 0xC8BC771B,
    0x5FF18686, 0xACFEF3FD, 0x7859EEDE, 0x7D11B6C1,
};
const uint32_t k_enc_span[ENC_SPAN_WORDS] = {
    0x8F7FFFFF, 0xB37939E5, 0xFC56C51B, 0xF4AF31B1, 0xFFFF1424,
};

// --- MSC and clock registers (EFR32ZG23) ------------------------------------

// MSC and CMU are secure peripherals. Whether the debugger reaches them at the
// secure alias (0x5003_xxxx) or the non-secure alias (0x4003_xxxx) depends on
// the running firmware's SMU config, so the base is detected at run time. The
// wrong alias reads as zero and ignores writes. Default to the secure alias.
static uint32_t g_msc_base   = 0x50030000u;
static uint32_t g_cmu_clken1 = 0x50008068u;

#define MSC_IPVERSION  (g_msc_base + 0x00)
#define MSC_WRITECTRL  (g_msc_base + 0x0C)
#define MSC_WRITECMD   (g_msc_base + 0x10)
#define MSC_ADDRB      (g_msc_base + 0x14)
#define MSC_WDATA      (g_msc_base + 0x18)
#define MSC_STATUS     (g_msc_base + 0x1C)
#define MSC_LOCK       (g_msc_base + 0x3C)
#define MSC_PAGELOCK1  (g_msc_base + 0x124)

#define MSC_LOCK_KEY          0x00001B71u
#define MSC_WRITECTRL_WREN    (1u << 0)
#define MSC_WRITECMD_ERASEPAGE (1u << 1)
#define MSC_WRITECMD_WRITEEND (1u << 2)
#define MSC_STATUS_BUSY       (1u << 0)
#define MSC_STATUS_INVADDR    (1u << 2)
#define MSC_STATUS_WDATAREADY (1u << 3)
#define MSC_STATUS_PENDING    (1u << 5)
#define MSC_STATUS_REGLOCK    (1u << 16)
#define MSC_PAGELOCK1_PAGE63  (1u << 31)

#define CMU_CLKEN1     (g_cmu_clken1)
#define CMU_CLKEN1_MSC (1u << 16)

#define MSC_IPVERSION_VALUE 0x00000002u
#define PERIPH_SECURE_OFFSET 0x10000000u  // secure alias sits 0x1000_0000 above

// Bootloader reset handoff, from nc-zwave-bootloader btl_reset_info.h. The app
// writes this cause to the first RAM word, then resets; the Gecko bootloader
// reads it on the way up and stays in communication mode.
#define SRAM_BASE 0x20000000u
#define BOOTLOADER_RESET_REASON_BOOTLOAD 0x0202u
#define BOOTLOADER_RESET_SIGNATURE_VALID 0xF00Fu

#define POLL_TIMEOUT_US 100000

// --- Read and classify ------------------------------------------------------

static bool span_is_ff(const uint32_t *w, int n)
{
    for (int i = 0; i < n; i++)
        if (w[i] != 0xFFFFFFFFu) return false;
    return true;
}

swd_result_t zg23_read_tokens(token_read_t *out, token_state_t *state)
{
    swd_result_t r = swd_mem_read_block(SIGN_SPAN_ADDR, out->sign, SIGN_SPAN_WORDS);
    if (r != SWD_OK) return r;
    r = swd_mem_read_block(ENC_SPAN_ADDR, out->enc, ENC_SPAN_WORDS);
    if (r != SWD_OK) return r;

    bool sign_match = memcmp(out->sign, k_sign_span, sizeof(k_sign_span)) == 0;
    bool enc_match = memcmp(out->enc, k_enc_span, sizeof(k_enc_span)) == 0;
    bool all_ff = span_is_ff(out->sign, SIGN_SPAN_WORDS) &&
                  span_is_ff(out->enc, ENC_SPAN_WORDS);

    if (sign_match && enc_match) *state = TOKENS_MATCH;
    else if (all_ff) *state = TOKENS_BLANK;
    else *state = TOKENS_DIFFER;
    return SWD_OK;
}

// --- MSC write --------------------------------------------------------------

static swd_result_t poll_status(uint32_t mask, uint32_t want)
{
    int64_t deadline = esp_timer_get_time() + POLL_TIMEOUT_US;
    for (;;) {
        uint32_t s;
        swd_result_t r = swd_mem_read32(MSC_STATUS, &s);
        if (r != SWD_OK) return r;
        if ((s & mask) == want) return SWD_OK;
        if (esp_timer_get_time() > deadline) return SWD_TIMEOUT;
    }
}

// One MSC burst: load ADDRB, stream words through WDATA, finish with WRITEEND.
static swd_result_t write_burst(uint32_t addr, const uint32_t *words, int n)
{
    swd_result_t r = swd_mem_write32(MSC_ADDRB, addr);
    if (r != SWD_OK) return r;

    uint32_t status;
    r = swd_mem_read32(MSC_STATUS, &status);
    if (r != SWD_OK) return r;
    if (status & MSC_STATUS_INVADDR) return SWD_FAULT;

    r = swd_mem_write32(MSC_WDATA, words[0]);
    if (r != SWD_OK) return r;
    for (int i = 1; i < n; i++) {
        r = poll_status(MSC_STATUS_WDATAREADY, MSC_STATUS_WDATAREADY);
        if (r != SWD_OK) return r;
        r = swd_mem_write32(MSC_WDATA, words[i]);
        if (r != SWD_OK) return r;
    }

    r = swd_mem_write32(MSC_WRITECMD, MSC_WRITECMD_WRITEEND);
    if (r != SWD_OK) return r;

    // em_msc.c waits for BUSY and PENDING to clear, then checks a second time.
    r = poll_status(MSC_STATUS_BUSY | MSC_STATUS_PENDING, 0);
    if (r != SWD_OK) return r;
    return poll_status(MSC_STATUS_BUSY | MSC_STATUS_PENDING, 0);
}

static swd_result_t set_bits(uint32_t addr, uint32_t bits)
{
    uint32_t v;
    swd_result_t r = swd_mem_read32(addr, &v);
    if (r != SWD_OK) return r;
    return swd_mem_write32(addr, v | bits);
}

static swd_result_t clear_bits(uint32_t addr, uint32_t bits)
{
    uint32_t v;
    swd_result_t r = swd_mem_read32(addr, &v);
    if (r != SWD_OK) return r;
    return swd_mem_write32(addr, v & ~bits);
}

#define STEP(call, msg)                       \
    do {                                      \
        swd_result_t _r = (call);             \
        if (_r != SWD_OK) {                   \
            snprintf(buf, sizeof(buf), "%s: %s", (msg), swd_strerror(_r)); \
            log_line(buf);                    \
            return _r;                        \
        }                                     \
    } while (0)

// Point the MSC/CMU bases at the alias the peripheral answers on. A RAZ/WI alias
// reads exactly 0; a reachable one reads the register, or bus garbage if its
// clock is gated. The controller makes the MSC secure-only, the bootloader
// leaves both aliases live, so prefer the secure alias whenever it responds.
static swd_result_t detect_periph_alias(zg23_log_fn log_line)
{
    char buf[96];
    uint32_t sec, nonsec;
    STEP(swd_mem_read32(0x40030000u + PERIPH_SECURE_OFFSET, &sec), "probe secure MSC");
    STEP(swd_mem_read32(0x40030000u, &nonsec), "probe non-secure MSC");
    if (sec != 0) {
        g_msc_base = 0x40030000u + PERIPH_SECURE_OFFSET;
        g_cmu_clken1 = 0x40008068u + PERIPH_SECURE_OFFSET;
        log_line("MSC via secure alias 0x50030000");
        return SWD_OK;
    }
    if (nonsec != 0) {
        g_msc_base = 0x40030000u;
        g_cmu_clken1 = 0x40008068u;
        log_line("MSC via non-secure alias 0x40030000");
        return SWD_OK;
    }
    log_line("MSC not reachable at either alias");
    return SWD_FAULT;
}

// Detect the alias, enable the MSC clock, clear page 63's lock, unlock
// registers, and set WREN. PAGELOCK1 bit 31 is set-only and clears on reset, so
// a locked page 63 forces a reset-and-halt before the flash op can run.
static swd_result_t msc_begin(zg23_log_fn log_line)
{
    char buf[96];
    STEP(detect_periph_alias(log_line), "detect MSC alias");
    STEP(set_bits(CMU_CLKEN1, CMU_CLKEN1_MSC), "enable MSC clock");

    uint32_t pagelock;
    STEP(swd_mem_read32(MSC_PAGELOCK1, &pagelock), "read PAGELOCK1");
    if (pagelock & MSC_PAGELOCK1_PAGE63) {
        log_line("page 63 locked, resetting to clear it");
        STEP(swd_reset_halt(), "reset-halt");
        STEP(set_bits(CMU_CLKEN1, CMU_CLKEN1_MSC), "re-enable MSC clock");
        STEP(swd_mem_read32(MSC_PAGELOCK1, &pagelock), "re-read PAGELOCK1");
        if (pagelock & MSC_PAGELOCK1_PAGE63) {
            log_line("page 63 still locked after reset");
            return SWD_FAULT;
        }
    }

    STEP(swd_mem_write32(MSC_LOCK, MSC_LOCK_KEY), "unlock MSC");
    uint32_t status;
    STEP(swd_mem_read32(MSC_STATUS, &status), "read STATUS");
    if (status & MSC_STATUS_REGLOCK) {
        log_line("MSC still register-locked after unlock key");
        return SWD_FAULT;
    }
    return set_bits(MSC_WRITECTRL, MSC_WRITECTRL_WREN);
}

static swd_result_t msc_end(zg23_log_fn log_line)
{
    char buf[96];
    STEP(clear_bits(MSC_WRITECTRL, MSC_WRITECTRL_WREN), "clear WREN");
    return swd_mem_write32(MSC_LOCK, 0);
}

swd_result_t zg23_write_tokens(zg23_log_fn log_line, bool *verified_out)
{
    char buf[96];
    *verified_out = false;

    token_read_t before;
    token_state_t state;
    STEP(swd_halt(), "halt");
    STEP(zg23_read_tokens(&before, &state), "read tokens");
    if (state == TOKENS_MATCH) {
        log_line("tokens already correct, nothing to do");
        *verified_out = true;
        return SWD_OK;
    }
    if (state != TOKENS_BLANK) {
        log_line("regions are neither blank nor the expected keys, refusing to write");
        return SWD_FAULT;
    }

    STEP(msc_begin(log_line), "prepare MSC");
    log_line("writing sign key span (X||Y)");
    STEP(write_burst(SIGN_SPAN_ADDR, k_sign_span, SIGN_SPAN_WORDS), "sign burst");
    log_line("writing encryption key span");
    STEP(write_burst(ENC_SPAN_ADDR, k_enc_span, ENC_SPAN_WORDS), "enc burst");
    STEP(msc_end(log_line), "finish MSC");

    token_read_t after;
    STEP(zg23_read_tokens(&after, &state), "read back");
    if (state != TOKENS_MATCH) {
        log_line("READBACK MISMATCH: restore failed, tokens are not correct");
        return SWD_FAULT;
    }
    log_line("VERIFIED: tokens read back correct");
    *verified_out = true;
    return SWD_OK;
}

swd_result_t zg23_erase_page(uint32_t page_addr, zg23_log_fn log_line)
{
    char buf[96];
    STEP(swd_halt(), "halt");
    STEP(msc_begin(log_line), "prepare MSC");

    STEP(swd_mem_write32(MSC_ADDRB, page_addr), "set ADDRB");
    uint32_t status;
    STEP(swd_mem_read32(MSC_STATUS, &status), "read STATUS");
    if (status & MSC_STATUS_INVADDR) {
        log_line("INVADDR: address is not in an erasable region");
        return SWD_FAULT;
    }
    STEP(swd_mem_write32(MSC_WRITECMD, MSC_WRITECMD_ERASEPAGE), "erase page");
    STEP(poll_status(MSC_STATUS_BUSY | MSC_STATUS_PENDING, 0), "wait for erase");
    STEP(poll_status(MSC_STATUS_BUSY | MSC_STATUS_PENDING, 0), "confirm idle");

    STEP(msc_end(log_line), "finish MSC");
    log_line("erase complete");
    return SWD_OK;
}

swd_result_t zg23_enter_bootloader(zg23_log_fn log_line)
{
    char buf[96];
    uint32_t cause = ((uint32_t)BOOTLOADER_RESET_SIGNATURE_VALID << 16)
                     | BOOTLOADER_RESET_REASON_BOOTLOAD;
    STEP(swd_mem_write32(SRAM_BASE, cause), "write reset cause");
    log_line("reset cause set, resetting to run the bootloader");
    return swd_reset_run();
}

swd_result_t zg23_write_span(uint32_t addr, const uint32_t *words, int n,
                             zg23_log_fn log_line)
{
    char buf[96];
    STEP(swd_halt(), "halt");
    STEP(msc_begin(log_line), "prepare MSC");
    STEP(write_burst(addr, words, n), "burst");
    STEP(msc_end(log_line), "finish MSC");

    uint32_t rb[64];
    STEP(swd_mem_read_block(addr, rb, n), "readback");
    for (int i = 0; i < n; i++) {
        if (rb[i] != words[i]) {
            log_line("readback mismatch");
            return SWD_FAULT;
        }
    }
    return SWD_OK;
}
