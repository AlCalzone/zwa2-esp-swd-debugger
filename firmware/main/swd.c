#include "swd.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

// --- DP / AP register model (ADIv5) -----------------------------------------

#define REQ_START (1u << 0)
#define REQ_APnDP (1u << 1)
#define REQ_RnW   (1u << 2)
#define REQ_PARK  (1u << 7)

#define ACK_OK    0x1
#define ACK_WAIT  0x2
#define ACK_FAULT 0x4

// DP register addresses (A[3:2] offsets)
#define DP_DPIDR    0x0
#define DP_ABORT    0x0
#define DP_CTRLSTAT 0x4
#define DP_SELECT   0x8
#define DP_RDBUFF   0xC

#define ABORT_CLEAR_ALL 0x0000001Eu  // STK{CMP,ERR,ORUN}CLR | WDERRCLR

#define CTRL_CDBGPWRUPREQ (1u << 28)
#define CTRL_CDBGPWRUPACK (1u << 29)
#define CTRL_CSYSPWRUPREQ (1u << 30)
#define CTRL_CSYSPWRUPACK (1u << 31)
#define CTRL_STICKYERR    (1u << 5)

// MEM-AP register addresses
#define AP_CSW 0x00
#define AP_TAR 0x04
#define AP_DRW 0x0C
#define AP_IDR 0xFC

// Word access, auto-increment, secure (HNONSEC=0). The RO DeviceEn bit reads
// back as set; writing it is ignored.
#define CSW_WORD_INC 0x23000052u

// Cortex-M debug registers
#define DHCSR 0xE000EDF0u
#define DEMCR 0xE000EDFCu
#define AIRCR 0xE000ED0Cu
#define DHCSR_HALT_REQ 0xA05F0003u  // DBGKEY | C_HALT | C_DEBUGEN
#define DHCSR_S_HALT   (1u << 17)
#define DEMCR_TRCENA       (1u << 24)
#define DEMCR_VC_CORERESET (1u << 0)
#define AIRCR_SYSRESETREQ  0x05FA0004u

#define XFER_RETRIES 40

// --- Bit-level SWD ----------------------------------------------------------

static bool s_dio_is_output;

static inline void half_period(void) { esp_rom_delay_us(SWD_HALF_PERIOD_US); }

static inline void dio_dir(bool out)
{
    if (out != s_dio_is_output) {
        gpio_set_direction(SWD_PIN_SWDIO, out ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
        s_dio_is_output = out;
    }
}

// One clock, SWCLK low->high->low. SWDIO holds whatever the caller left it at,
// so this doubles as a turnaround (SWDIO input) and an idle bit (SWDIO low).
static inline void clk_pulse(void)
{
    half_period();
    gpio_set_level(SWD_PIN_SWCLK, 1);
    half_period();
    gpio_set_level(SWD_PIN_SWCLK, 0);
}

static inline void write_bit(int b)
{
    gpio_set_level(SWD_PIN_SWDIO, b & 1);
    clk_pulse();
}

// Target updates SWDIO on the falling edge, so wait a half period for it to
// settle, sample while SWCLK is still low, then clock the next bit out.
static inline int read_bit(void)
{
    half_period();
    int b = gpio_get_level(SWD_PIN_SWDIO);
    gpio_set_level(SWD_PIN_SWCLK, 1);
    half_period();
    gpio_set_level(SWD_PIN_SWCLK, 0);
    return b;
}

static void idle_bits(int n)
{
    dio_dir(true);
    gpio_set_level(SWD_PIN_SWDIO, 0);
    for (int i = 0; i < n; i++) clk_pulse();
}

static void line_reset(void)
{
    dio_dir(true);
    gpio_set_level(SWD_PIN_SWDIO, 1);
    for (int i = 0; i < 56; i++) clk_pulse();
}

// --- One SWD transfer -------------------------------------------------------

static swd_result_t swd_transfer(int ap, int rnw, uint8_t addr, uint32_t *data)
{
    int a2 = (addr >> 2) & 1;
    int a3 = (addr >> 3) & 1;
    int parity = ap ^ rnw ^ a2 ^ a3;
    uint8_t req = REQ_START | REQ_PARK |
                  (ap << 1) | (rnw << 2) | (a2 << 3) | (a3 << 4) | (parity << 5);

    dio_dir(true);
    for (int i = 0; i < 8; i++) write_bit((req >> i) & 1);

    // Turnaround, then read the 3-bit ACK.
    dio_dir(false);
    clk_pulse();
    int ack = 0;
    for (int i = 0; i < 3; i++) ack |= read_bit() << i;

    if (ack == ACK_OK) {
        if (rnw) {
            uint32_t v = 0;
            int par = 0;
            for (int i = 0; i < 32; i++) {
                int b = read_bit();
                v |= (uint32_t)b << i;
                par ^= b;
            }
            int p = read_bit();
            clk_pulse();          // turnaround back to host
            idle_bits(8);
            if (data) *data = v;
            return (p == (par & 1)) ? SWD_OK : SWD_PARITY;
        }
        clk_pulse();              // turnaround back to host
        dio_dir(true);
        uint32_t v = data ? *data : 0;
        int par = 0;
        for (int i = 0; i < 32; i++) {
            int b = (v >> i) & 1;
            write_bit(b);
            par ^= b;
        }
        write_bit(par & 1);
        idle_bits(8);
        return SWD_OK;
    }

    // WAIT and FAULT leave one turnaround owed before the host may drive again.
    clk_pulse();
    idle_bits(8);
    if (ack == ACK_WAIT) return SWD_WAIT;
    if (ack == ACK_FAULT) return SWD_FAULT;
    return SWD_NOACK;
}

// --- DP / AP helpers with WAIT/FAULT retry ----------------------------------

static swd_result_t dp_raw(int rnw, uint8_t addr, uint32_t *data)
{
    return swd_transfer(0, rnw, addr, data);
}

static swd_result_t xfer_retry(int ap, int rnw, uint8_t addr, uint32_t *data)
{
    swd_result_t r = SWD_NOACK;
    for (int i = 0; i < XFER_RETRIES; i++) {
        r = swd_transfer(ap, rnw, addr, data);
        if (r == SWD_OK || r == SWD_PARITY) return r;
        if (r == SWD_FAULT) {
            uint32_t clr = ABORT_CLEAR_ALL;
            dp_raw(0, DP_ABORT, &clr);
        }
        // WAIT and cleared FAULT both fall through to another attempt.
    }
    return r;
}

static swd_result_t dp_read(uint8_t addr, uint32_t *val) { return xfer_retry(0, 1, addr, val); }
static swd_result_t dp_write(uint8_t addr, uint32_t val) { return xfer_retry(0, 0, addr, &val); }

static swd_result_t ap_read(uint8_t reg, uint32_t *val)
{
    uint8_t bank = reg & 0xF0;
    if (bank) dp_write(DP_SELECT, bank);
    uint32_t dummy;
    swd_result_t r = xfer_retry(1, 1, reg & 0x0C, &dummy);  // posts the read
    if (r == SWD_OK) r = dp_read(DP_RDBUFF, val);           // returns the value
    if (bank) dp_write(DP_SELECT, 0);
    return r;
}

static swd_result_t ap_write(uint8_t reg, uint32_t val)
{
    uint8_t bank = reg & 0xF0;
    if (bank) dp_write(DP_SELECT, bank);
    swd_result_t r = xfer_retry(1, 0, reg & 0x0C, &val);
    if (bank) dp_write(DP_SELECT, 0);
    return r;
}

// --- Public API -------------------------------------------------------------

const char *swd_strerror(swd_result_t r)
{
    switch (r) {
    case SWD_OK:      return "ok";
    case SWD_WAIT:    return "wait";
    case SWD_FAULT:   return "fault";
    case SWD_PARITY:  return "parity";
    case SWD_NOACK:   return "no-ack";
    case SWD_TIMEOUT: return "timeout";
    }
    return "?";
}

void swd_gpio_init(void)
{
    gpio_config_t clk = {
        .pin_bit_mask = 1ULL << SWD_PIN_SWCLK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&clk);
    gpio_set_level(SWD_PIN_SWCLK, 0);

    // Pull-up so the line reads high while released during turnaround.
    gpio_config_t dio = {
        .pin_bit_mask = 1ULL << SWD_PIN_SWDIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&dio);
    s_dio_is_output = true;
    gpio_set_level(SWD_PIN_SWDIO, 1);
}

swd_result_t swd_clear_errors(void)
{
    uint32_t stat;
    swd_result_t r = dp_read(DP_CTRLSTAT, &stat);
    if (r != SWD_OK) return r;
    if (stat & CTRL_STICKYERR) return dp_write(DP_ABORT, ABORT_CLEAR_ALL);
    return SWD_OK;
}

swd_result_t swd_connect(uint32_t *idcode_out)
{
    swd_gpio_init();

    // JTAG-to-SWD: reset, 0xE79E switch (LSB first), reset, idle.
    line_reset();
    uint16_t sw = 0xE79E;
    dio_dir(true);
    for (int i = 0; i < 16; i++) write_bit((sw >> i) & 1);
    line_reset();
    idle_bits(4);

    uint32_t id = 0;
    swd_result_t r = dp_read(DP_DPIDR, &id);
    if (r != SWD_OK) return r;
    if (idcode_out) *idcode_out = id;

    dp_write(DP_ABORT, ABORT_CLEAR_ALL);
    dp_write(DP_SELECT, 0);

    r = dp_write(DP_CTRLSTAT, CTRL_CDBGPWRUPREQ | CTRL_CSYSPWRUPREQ);
    if (r != SWD_OK) return r;
    int64_t deadline = esp_timer_get_time() + 100000;  // 100 ms
    for (;;) {
        uint32_t stat;
        r = dp_read(DP_CTRLSTAT, &stat);
        if (r != SWD_OK) return r;
        if ((stat & (CTRL_CDBGPWRUPACK | CTRL_CSYSPWRUPACK)) ==
            (CTRL_CDBGPWRUPACK | CTRL_CSYSPWRUPACK))
            break;
        if (esp_timer_get_time() > deadline) return SWD_TIMEOUT;
    }

    return ap_write(AP_CSW, CSW_WORD_INC);
}

swd_result_t swd_read_ap_idr(uint32_t *idr_out) { return ap_read(AP_IDR, idr_out); }

swd_result_t swd_mem_read32(uint32_t addr, uint32_t *val)
{
    swd_result_t r = ap_write(AP_TAR, addr);
    if (r != SWD_OK) return r;
    return ap_read(AP_DRW, val);
}

swd_result_t swd_mem_write32(uint32_t addr, uint32_t val)
{
    swd_result_t r = ap_write(AP_TAR, addr);
    if (r != SWD_OK) return r;
    r = ap_write(AP_DRW, val);
    if (r != SWD_OK) return r;
    uint32_t flush;                 // push the posted AP write through
    return dp_read(DP_RDBUFF, &flush);
}

swd_result_t swd_mem_read_block(uint32_t addr, uint32_t *buf, int words)
{
    for (int i = 0; i < words; i++) {
        swd_result_t r = swd_mem_read32(addr + 4 * i, &buf[i]);
        if (r != SWD_OK) return r;
    }
    return SWD_OK;
}

swd_result_t swd_is_halted(bool *halted)
{
    uint32_t dhcsr;
    swd_result_t r = swd_mem_read32(DHCSR, &dhcsr);
    if (r != SWD_OK) return r;
    *halted = (dhcsr & DHCSR_S_HALT) != 0;
    return SWD_OK;
}

swd_result_t swd_halt(void)
{
    swd_result_t r = swd_mem_write32(DHCSR, DHCSR_HALT_REQ);
    if (r != SWD_OK) return r;
    int64_t deadline = esp_timer_get_time() + 100000;
    for (;;) {
        bool halted;
        r = swd_is_halted(&halted);
        if (r != SWD_OK) return r;
        if (halted) return SWD_OK;
        if (esp_timer_get_time() > deadline) return SWD_TIMEOUT;
    }
}

swd_result_t swd_reset_run(void)
{
    // Do not halt on reset, and release the debug halt so the core runs.
    swd_result_t r = swd_mem_write32(DEMCR, 0);
    if (r != SWD_OK) return r;
    r = swd_mem_write32(DHCSR, 0xA05F0000);  // DBGKEY, C_DEBUGEN and C_HALT clear
    if (r != SWD_OK) return r;
    return swd_mem_write32(AIRCR, AIRCR_SYSRESETREQ);
}

swd_result_t swd_reset_halt(void)
{
    // Halt on the reset vector so no code runs after the reset.
    swd_result_t r = swd_mem_write32(DEMCR, DEMCR_TRCENA | DEMCR_VC_CORERESET);
    if (r != SWD_OK) return r;
    r = swd_mem_write32(DHCSR, DHCSR_HALT_REQ);
    if (r != SWD_OK) return r;
    r = swd_mem_write32(AIRCR, AIRCR_SYSRESETREQ);
    if (r != SWD_OK) return r;

    esp_rom_delay_us(2000);
    // SYSRESETREQ leaves the debug port powered but resets the MEM-AP config.
    r = ap_write(AP_CSW, CSW_WORD_INC);
    if (r != SWD_OK) return r;

    int64_t deadline = esp_timer_get_time() + 200000;
    for (;;) {
        bool halted;
        r = swd_is_halted(&halted);
        if (r == SWD_OK && halted) return SWD_OK;
        if (esp_timer_get_time() > deadline) return SWD_TIMEOUT;
    }
}
