#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_console.h"
#include "esp_log.h"

#include "swd.h"
#include "zg23.h"

static const char *TAG = "swd";

static uint8_t span_byte(const uint32_t *words, int i)
{
    return (words[i / 4] >> (8 * (i % 4))) & 0xFF;
}

static void print_bytes(const char *label, uint32_t addr, const uint32_t *words,
                        int first, int len)
{
    printf("%-28s 0x%08" PRIX32 "  ", label, addr);
    for (int i = 0; i < len; i++) printf("%02x", span_byte(words, first + i));
    printf("\n");
}

static bool ensure_connected(void)
{
    uint32_t idcode;
    swd_result_t r = swd_connect(&idcode);
    if (r != SWD_OK) {
        printf("connect failed: %s (check wiring and target power)\n", swd_strerror(r));
        return false;
    }
    return true;
}

static int cmd_id(int argc, char **argv)
{
    uint32_t idcode;
    swd_result_t r = swd_connect(&idcode);
    if (r != SWD_OK) {
        printf("connect failed: %s\n", swd_strerror(r));
        return 1;
    }
    uint32_t idr = 0;
    swd_read_ap_idr(&idr);
    printf("DPIDR  0x%08" PRIX32 "\n", idcode);
    printf("AP IDR 0x%08" PRIX32 "\n", idr);
    return 0;
}

static int cmd_read(int argc, char **argv)
{
    if (!ensure_connected()) return 1;
    if (swd_halt() != SWD_OK) printf("warning: halt failed, reading a running core\n");

    token_read_t t;
    token_state_t state;
    swd_result_t r = zg23_read_tokens(&t, &state);
    if (r != SWD_OK) {
        printf("read failed: %s\n", swd_strerror(r));
        return 1;
    }
    print_bytes("SIGNED_BOOTLOADER_KEY_X", TOK_X_ADDR, t.sign, 0, 32);
    print_bytes("SIGNED_BOOTLOADER_KEY_Y", TOK_Y_ADDR, t.sign, 32, 32);
    print_bytes("SECURE_BOOTLOADER_KEY", TOK_ENC_ADDR, t.enc, 2, 16);
    switch (state) {
    case TOKENS_MATCH:
        printf("state: MATCH - byte order on silicon matches the plan\n");
        break;
    case TOKENS_BLANK:
        printf("state: BLANK - all 0xFF, this is a bricked board\n");
        break;
    case TOKENS_DIFFER:
        printf("state: DIFFERS - neither the expected keys nor blank\n");
        break;
    }
    return 0;
}

static int cmd_dump(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: dump <hex_addr> <word_count>\n");
        return 1;
    }
    uint32_t addr = strtoul(argv[1], NULL, 16);
    int words = atoi(argv[2]);
    if (words < 1 || words > 64) {
        printf("word_count must be 1..64\n");
        return 1;
    }
    if (!ensure_connected()) return 1;

    uint32_t buf[64];
    swd_result_t r = swd_mem_read_block(addr, buf, words);
    if (r != SWD_OK) {
        printf("read failed: %s\n", swd_strerror(r));
        return 1;
    }
    for (int i = 0; i < words; i++)
        printf("0x%08" PRIX32 "  0x%08" PRIX32 "\n", addr + 4 * i, buf[i]);
    return 0;
}

static int cmd_halt(int argc, char **argv)
{
    if (!ensure_connected()) return 1;
    swd_result_t r = swd_halt();
    printf("halt: %s\n", swd_strerror(r));
    return r == SWD_OK ? 0 : 1;
}

static int cmd_reset(int argc, char **argv)
{
    if (!ensure_connected()) return 1;
    swd_result_t r = swd_reset_halt();
    printf("reset-halt: %s\n", swd_strerror(r));
    return r == SWD_OK ? 0 : 1;
}

static void write_log(const char *line) { printf("  %s\n", line); }

static int cmd_erase(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
        printf("Erases the whole 8KB lockbits page 63, which blanks the tokens\n");
        printf("AND the DSK and QR data. For write-path testing only.\n");
        printf("Type: erase confirm\n");
        return 1;
    }
    if (!ensure_connected()) return 1;
    swd_result_t r = zg23_erase_page(LOCKBITS_PAGE_ADDR, write_log);
    if (r != SWD_OK) {
        printf("erase failed: %s\n", swd_strerror(r));
        return 1;
    }
    token_read_t t;
    token_state_t state;
    if (zg23_read_tokens(&t, &state) == SWD_OK)
        printf("post-erase token state: %s\n",
               state == TOKENS_BLANK ? "BLANK" : state == TOKENS_MATCH ? "MATCH" : "DIFFERS");
    return 0;
}

static int cmd_write(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
        printf("This writes the bootloader key tokens and cannot be undone.\n");
        printf("It only proceeds when all three regions read 0xFF.\n");
        printf("Type: write confirm\n");
        return 1;
    }
    if (!ensure_connected()) return 1;

    bool verified = false;
    swd_result_t r = zg23_write_tokens(write_log, &verified);
    if (r != SWD_OK || !verified) {
        printf("write did not complete cleanly: %s\n", swd_strerror(r));
        return 1;
    }
    printf("done\n");
    return 0;
}

static int cmd_wr(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: wr <hexaddr> <hexword> [hexword...]\n");
        return 1;
    }
    uint32_t addr = strtoul(argv[1], NULL, 16);
    int n = argc - 2;
    if (n > 64) {
        printf("at most 64 words per call\n");
        return 1;
    }
    uint32_t end = LOCKBITS_PAGE_ADDR + 0x2000u;
    if ((addr & 3u) || addr < LOCKBITS_PAGE_ADDR || addr + 4u * n > end) {
        printf("addr must be word-aligned within page 63 (0x%08" PRIX32 "..0x%08" PRIX32 ")\n",
               (uint32_t)LOCKBITS_PAGE_ADDR, end);
        return 1;
    }
    uint32_t words[64];
    for (int i = 0; i < n; i++) words[i] = strtoul(argv[2 + i], NULL, 16);

    if (!ensure_connected()) return 1;
    swd_result_t r = zg23_write_span(addr, words, n, write_log);
    printf("wr %d word(s) @ 0x%08" PRIX32 ": %s\n", n, addr, swd_strerror(r));
    return r == SWD_OK ? 0 : 1;
}

static int cmd_run(int argc, char **argv)
{
    if (!ensure_connected()) return 1;
    swd_result_t r = swd_reset_run();
    printf("reset-run: %s\n", swd_strerror(r));
    return r == SWD_OK ? 0 : 1;
}

static int cmd_bootloader(int argc, char **argv)
{
    if (!ensure_connected()) return 1;
    swd_result_t r = zg23_enter_bootloader(write_log);
    if (r != SWD_OK) {
        printf("failed: %s\n", swd_strerror(r));
        return 1;
    }
    printf("target reset into bootloader; use 'halt' to catch and probe it\n");
    return 0;
}

static void register_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "id",    .help = "connect and print DPIDR and AP IDR", .func = cmd_id},
        {.command = "read",  .help = "read and classify the key tokens", .func = cmd_read},
        {.command = "dump",  .help = "dump <hex_addr> <word_count>", .func = cmd_dump},
        {.command = "halt",  .help = "halt the target core", .func = cmd_halt},
        {.command = "reset", .help = "reset and halt the target core", .func = cmd_reset},
        {.command = "run", .help = "reset and let the target run (exit bootloader)", .func = cmd_run},
        {.command = "write", .help = "restore blank tokens (needs: write confirm)", .func = cmd_write},
        {.command = "wr", .help = "wr <hexaddr> <hexword...> : write a flash span in page 63", .func = cmd_wr},
        {.command = "erase", .help = "erase lockbits page 63 (needs: erase confirm)", .func = cmd_erase},
        {.command = "bootloader", .help = "reset the target into its OTW bootloader", .func = cmd_bootloader},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}

void app_main(void)
{
    swd_gpio_init();
    ESP_LOGI(TAG, "SWD token tool: SWCLK=GPIO%d SWDIO=GPIO%d", SWD_PIN_SWCLK, SWD_PIN_SWDIO);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "swd>";
    repl_config.max_cmdline_args = 70;      // wr takes an address plus up to 64 words
    repl_config.max_cmdline_length = 768;
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_config, &repl));
    esp_console_register_help_command();
    register_cmds();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
