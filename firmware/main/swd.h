#pragma once

#include <stdint.h>
#include <stdbool.h>

// Bit-banged SWD (ARM ADIv5) master over two GPIOs. SWCLK idles low.
// GPIO5/GPIO6 are ordinary IO on both ESP32-C3 and ESP32-S3; override at
// build time with -DSWD_PIN_SWCLK / -DSWD_PIN_SWDIO if wired differently.
#ifndef SWD_PIN_SWCLK
#define SWD_PIN_SWCLK 5
#endif
#ifndef SWD_PIN_SWDIO
#define SWD_PIN_SWDIO 6
#endif

// Half clock period. 2us gives ~200 kHz, slow enough to survive jumper wires.
#ifndef SWD_HALF_PERIOD_US
#define SWD_HALF_PERIOD_US 2
#endif

typedef enum {
    SWD_OK = 0,
    SWD_WAIT,     // target asked to retry
    SWD_FAULT,    // target flagged a sticky error
    SWD_PARITY,   // read data parity mismatch
    SWD_NOACK,    // no valid ACK, wiring or protocol failure
    SWD_TIMEOUT,  // a status poll never settled
} swd_result_t;

const char *swd_strerror(swd_result_t r);

void swd_gpio_init(void);

// Line reset, JTAG-to-SWD switch, power up the debug domain, init the MEM-AP.
// Returns the DPIDR in idcode_out on success.
swd_result_t swd_connect(uint32_t *idcode_out);
swd_result_t swd_read_ap_idr(uint32_t *idr_out);

swd_result_t swd_mem_read32(uint32_t addr, uint32_t *val);
swd_result_t swd_mem_write32(uint32_t addr, uint32_t val);
swd_result_t swd_mem_read_block(uint32_t addr, uint32_t *buf, int words);

swd_result_t swd_halt(void);
swd_result_t swd_reset_halt(void);
// Release the halt and system-reset the core so it runs free afterwards.
swd_result_t swd_reset_run(void);
swd_result_t swd_is_halted(bool *halted);

// Read CTRL/STAT and clear any sticky error flags via ABORT.
swd_result_t swd_clear_errors(void);
