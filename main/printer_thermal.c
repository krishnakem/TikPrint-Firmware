/*
 * Real thermal printer, driven over a serial wire with ESC/POS commands.
 *
 * Wiring (one direction only: the board talks, the printer listens):
 *   board pin PRINTER_TX_GPIO  ->  printer TTL socket, pin labelled "TX" (green wire)
 *   board GND                  ->  printer TTL socket, pin labelled "GND" (black wire)
 * The printer (EM8026-CUT) labels its pins from the board's point of view: "TX" is
 * where the board's transmit wire goes. The pin labelled "RX" does not work.
 * The printer needs its own power supply; the board cannot power it.
 *
 * Sending a ticket over a serial wire is slow (a tall image can take over a
 * minute), and print() only returns when the whole ticket has been sent. That is
 * fine: it is called by the print queue's background task (print_queue.c), never
 * by the web server. The queue also makes sure only one thing prints at a time.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"

#include "printer.h"

static const char *TAG = "thermal_printer";

/* ---- Settings you may need to change for your printer ---- */
#define PRINTER_UART       UART_NUM_1
#define PRINTER_TX_GPIO    17      // board pin wired to the printer's "TX" pin (see above)
#define PRINTER_BAUD       115200  // must match the printer (its self-test page shows it)
#define PRINTER_HAS_CUTTER 1       // 1 = cut the paper after each ticket
#define FEED_LINES_AFTER   4       // blank lines after the ticket, so it clears the tear bar

/* Image rows sent per command. Small bands keep the printer's memory from overflowing. */
#define BAND_ROWS 64

#define TEST_LINE_MAX 64

static void send(const void *data, size_t len)
{
    uart_write_bytes(PRINTER_UART, data, len);   // waits until the bytes are on their way
}

static void cmd(const char *bytes, size_t len)
{
    send(bytes, len);
}
#define CMD(literal) cmd(literal, sizeof(literal) - 1)

/*
 * The printer's built-in font only knows plain ASCII plus a few extras, but
 * tickets arrive as UTF-8. Swap the characters we know for the closest thing
 * the printer has, and print '?' for the rest.
 */
static void send_text(const char *s)
{
    char out[96];
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int extra;
        if (*p < 0x80)                { cp = *p;        extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else                          { cp = '?';       extra = 0; }
        p++;
        while (extra-- > 0 && (*p & 0xC0) == 0x80) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }

        const char *rep = NULL;
        char one[2] = { '?', 0 };
        if (cp < 0x80) {
            one[0] = (char)cp;
        } else {
            switch (cp) {
            case 0x00B0: one[0] = (char)0xF8; break;            // degree sign (code page 437)
            case 0x00B7: one[0] = (char)0xFA; break;            // middle dot
            case 0x2022: one[0] = '*'; break;                   // bullet
            case 0x2018: case 0x2019: one[0] = '\''; break;     // curly single quotes
            case 0x201C: case 0x201D: one[0] = '"'; break;      // curly double quotes
            case 0x2013: case 0x2014: one[0] = '-'; break;      // dashes
            case 0x2026: rep = "..."; break;
            case 0x2610: rep = "[ ]"; break;                    // empty checkbox
            case 0x2611: case 0x2612: rep = "[x]"; break;       // ticked checkbox
            default: break;
            }
        }
        if (!rep) {
            rep = one;
        }
        size_t rl = strlen(rep);
        if (n + rl > sizeof(out)) {
            send(out, n);
            n = 0;
        }
        memcpy(out + n, rep, rl);
        n += rl;
    }
    send(out, n);
}

static void send_image(const uint8_t *img, uint16_t rows)
{
    for (uint16_t y = 0; y < rows; y += BAND_ROWS) {
        uint16_t h = rows - y < BAND_ROWS ? rows - y : BAND_ROWS;
        // GS v 0: "print a raster image". Then width in bytes and height in rows, low byte first.
        const uint8_t head[] = { 0x1D, 'v', '0', 0,
                                 PRINTER_WIDTH_BYTES & 0xFF, PRINTER_WIDTH_BYTES >> 8,
                                 h & 0xFF, h >> 8 };
        send(head, sizeof(head));
        send(img + (size_t)y * PRINTER_WIDTH_BYTES, (size_t)h * PRINTER_WIDTH_BYTES);
    }
}

static void finish_ticket(void)
{
    const char feed[] = { 0x1B, 'd', FEED_LINES_AFTER };   // ESC d n: feed n lines
    send(feed, sizeof(feed));
#if PRINTER_HAS_CUTTER
    CMD("\x1DV\x01");                                      // GS V 1: partial cut
#endif
}

static void send_job(const printer_job_t *job)
{
    char line[32];
    CMD("\x1B@");        // ESC @: reset the printer to its defaults
    CMD("\x1Bt\x00");    // ESC t 0: code page 437 (matches send_text above)

    // Normally the page has drawn the text into the image, in the font picked there.
    // The printer's built-in font is only used when it hasn't (e.g. a ticket sent with curl).
    if (!job->text_in_image) {
        snprintf(line, sizeof(line), "#%04u", (unsigned)job->number);
        send_text(line);
        if (job->source[0]) {
            send_text("  ");
            send_text(job->source);
        }
        send_text("\n");

        if (job->title[0]) {
            CMD("\x1B" "E\x01" "\x1D!\x11");   // bold on, double width + height
            send_text(job->title);
            send_text("\n");
            CMD("\x1D!\x00" "\x1B" "E\x00");   // back to normal
        }
        if (job->body[0]) {
            send_text(job->body);
            send_text("\n");
        }
    }
    if (job->image && job->image_rows) {
        send_image(job->image, job->image_rows);
    }
    finish_ticket();
}

/* Print the test line. The board also listens on its own TX pin, so it can check that the
 * bytes really appear on the pin (this says nothing about the wire or the printer). */
static void send_test_line(const char *text)
{
    char echo[TEST_LINE_MAX + 8];
    uart_flush_input(PRINTER_UART);
    CMD("\x1B@");
    send_text(text);
    CMD("\n\n");   // no cut, so trying several settings in a row wastes little paper
    uart_wait_tx_done(PRINTER_UART, portMAX_DELAY);
    int want = 2 + (int)strlen(text) + 2;
    int got = uart_read_bytes(PRINTER_UART, echo, sizeof(echo), pdMS_TO_TICKS(100));
    bool same = got == want && memcmp(echo + 2, text, strlen(text)) == 0;
    ESP_LOGI(TAG, "pin check: sent %d bytes, saw %d on the pin: %s", want, got,
             same ? "OK, the board is sending" : "MISMATCH");
}

static esp_err_t thermal_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = PRINTER_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    // 256 = smallest receive buffer the driver accepts. The printer's TX is not wired; the
    // receive side listens on the board's own TX pin, only for the pin check in the test print.
    ESP_RETURN_ON_ERROR(uart_driver_install(PRINTER_UART, 256, 0, 0, NULL, 0), TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(PRINTER_UART, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(PRINTER_UART, PRINTER_TX_GPIO, PRINTER_TX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    ESP_RETURN_ON_ERROR(printer_fake_driver.init(), TAG, "last-ticket store");
    ESP_LOGI(TAG, "printer on GPIO%d at %d baud", PRINTER_TX_GPIO, PRINTER_BAUD);
    return ESP_OK;
}

static esp_err_t thermal_print(const printer_job_t *job)
{
    // The fake driver logs the ticket and keeps a copy of it for /api/last-ticket.
    // That copy is made now, so "last ticket" is the last one that STARTED printing.
    printer_fake_driver.print(job);
    ESP_LOGI(TAG, "sending ticket #%04u to the printer", (unsigned)job->number);
    send_job(job);
    uart_wait_tx_done(PRINTER_UART, portMAX_DELAY);
    ESP_LOGI(TAG, "done");
    return ESP_OK;
}

esp_err_t printer_thermal_test(int baud, int pin, int invert)
{
    static int s_pin = PRINTER_TX_GPIO, s_invert = 0;
    char line[TEST_LINE_MAX];
    if (baud > 0) {
        uart_set_baudrate(PRINTER_UART, baud);
    } else {
        uint32_t now = 0;
        uart_get_baudrate(PRINTER_UART, &now);
        baud = (int)now;
    }
    if (pin >= 0 && pin != s_pin) {
        uart_set_pin(PRINTER_UART, pin, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        s_pin = pin;
    }
    if (invert >= 0) {
        // Inverted = the wire rests at 0 V instead of 3.3 V. RS-232 printers expect that.
        uart_set_line_inverse(PRINTER_UART, invert ? UART_SIGNAL_TXD_INV : UART_SIGNAL_INV_DISABLE);
        s_invert = invert ? 1 : 0;
    }
    snprintf(line, sizeof(line), "TikPrint test: %d baud, pin %d%s\n",
             baud, s_pin, s_invert ? ", inverted" : "");
    ESP_LOGI(TAG, "test print: %d baud, GPIO%d%s", baud, s_pin, s_invert ? ", inverted" : "");
    send_test_line(line);
    return ESP_OK;
}

static const printer_job_t *thermal_last_job(void)
{
    return printer_fake_driver.last_job();
}

static void thermal_last_job_lock(void)
{
    printer_fake_driver.last_job_lock();
}

static void thermal_last_job_unlock(void)
{
    printer_fake_driver.last_job_unlock();
}

const printer_driver_t printer_thermal_driver = {
    .name = "thermal (ESC/POS over serial)",
    .init = thermal_init,
    .print = thermal_print,
    // The last ticket lives in the fake driver's copy (see thermal_print), so reading it goes there too.
    .last_job = thermal_last_job,
    .last_job_lock = thermal_last_job_lock,
    .last_job_unlock = thermal_last_job_unlock,
};
