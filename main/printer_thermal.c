/*
 * Real thermal printer, driven over a serial wire with ESC/POS commands.
 *
 * Wiring (printer TTL socket):
 *   board pin PRINTER_TX_GPIO  ->  pin labelled "TX" (green wire)    the board talks
 *   board pin PRINTER_RX_GPIO  <-  pin labelled "RX" (yellow wire)   the printer answers
 *   board GND                  --  pin labelled "GND" (black wire)
 * The printer (EM8026-CUT) labels its pins from the board's point of view: "TX" is
 * where the board's transmit wire goes, "RX" is where the board listens.
 * The yellow wire must not carry more than 3.3 V into the board (see README).
 * The printer needs its own power supply; the board cannot power it.
 *
 * The yellow wire is optional. It lets the board ask the printer how it is (paper,
 * cover, errors) with the ESC/POS "real-time status" command DLE EOT n. Without it
 * no answer ever comes, the status is "unknown", and tickets print just the same.
 *
 * Sending a ticket over a serial wire is slow (a tall image can take over a
 * minute), and print() only returns when the whole ticket has been sent. That is
 * fine: it is called by the print queue's background task (print_queue.c), never
 * by the web server. The queue also makes sure only one thing prints at a time.
 *
 * The serial line is shared by tickets, the test line and status questions, which
 * come from different tasks. Each takes s_uart_lock for its whole conversation, so
 * a status question can never land in the middle of a ticket's image data.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "printer.h"
#include "clock.h"
#include "settings.h"

static const char *TAG = "thermal_printer";

/* ---- Settings you may need to change for your printer ---- */
#define PRINTER_UART       UART_NUM_1
#define PRINTER_TX_GPIO    17      // board pin wired to the printer's "TX" pin (see above)
#define PRINTER_RX_GPIO    18      // board pin wired to the printer's "RX" pin: the printer's answers. 3.3 V only!
#define PRINTER_BAUD       115200  // must match the printer (its self-test page shows it)
#define PRINTER_HAS_CUTTER 1       // 1 = cut the paper after each ticket
#define FEED_LINES_AFTER   4       // blank lines after the ticket, so it clears the tear bar
#define HEADER_DATE_FORMAT "%b %d, %Y"     // header line of built-in font tickets, e.g. "Nov 20, 2026" (strftime patterns)
#define HEADER_TIME_FORMAT "%I:%M %p %Z"   //                                        e.g. "10:05 PM EST"

/* How an image is sent: BAND_ROWS rows per command, and an optional pause after each band.
 * The pause is PACE_US_PER_DOT microseconds for every black dot in the band, so dark bands
 * get long pauses and text gets none. 0 = no pauses.
 *
 * Why this matters: on this printer, large dark areas come out squashed (the paper does not
 * advance a full step for every row), while light areas print true to size. Measurements so
 * far (/api/diag/printer-pace prints a strip to compare settings) show the printer takes the
 * data without trouble, so it is not about the board sending too fast for the printer's
 * memory. Giving the printer time between dark rows helped, which points at its power supply
 * struggling when many dots are heated in a row. */
#define BAND_ROWS       64
#define PACE_US_PER_DOT 0

/* Blank rows printed after a ticket's image: a bottom margin about as tall as the top one.
 * (The top margin comes for free: it is the paper between the print head and the cutter.) */
#define FOOTER_ROWS 64

/* Pace test (/api/diag/printer-pace): the most settings one strip can compare, and the tallest block. */
#define PACE_TEST_MAX_ROWS 200

#define TEST_LINE_MAX 64

/* ---- Status questions (DLE EOT n) ---- */
#define STATUS_REPLY_TIMEOUT_MS 100   // how long to wait for each one-byte answer. A healthy printer takes a few ms
#define STATUS_RX_BUFFER        256   // smallest receive buffer the UART driver accepts; answers are 1 byte each

/* Every real status byte has these bits fixed: 0 . . 1 . . 1 0. Anything else is noise
 * (wrong speed, loose wire, or the XON/XOFF bytes some printers send) and is ignored. */
#define STATUS_FIXED_MASK  0x93
#define STATUS_FIXED_VALUE 0x12

/* What the bits of each answer mean (Epson ESC/POS, "DLE EOT n"). */
#define ST1_OFFLINE        0x08   // n=1 printer:       offline
#define ST2_COVER_OPEN     0x04   // n=2 offline cause: cover is open
#define ST2_PAPER_END_STOP 0x20   //                    printing stopped because the paper ran out
#define ST2_ERROR          0x40   //                    an error occurred
#define ST3_CUTTER_ERROR   0x08   // n=3 error cause:   auto-cutter error
#define ST3_OTHER_ERRORS   0x64   //                    mechanical, unrecoverable or overheated head
#define ST4_PAPER_NEAR_END 0x0C   // n=4 paper sensor:  roll nearly finished
#define ST4_PAPER_END      0x60   //                    no paper

static SemaphoreHandle_t s_uart_lock;   // held for one whole conversation with the printer

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

/* Send image rows band_rows at a time. After each band, wait us_per_dot microseconds per black
 * dot in it. The task can only sleep in steps of one tick (10 ms), so short waits are saved
 * up and taken together once they add up to a tick. */
static void send_image_paced(const uint8_t *img, uint16_t rows, int band_rows, int us_per_dot)
{
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;
    int64_t owed_us = 0;
    for (uint16_t y = 0; y < rows; y += band_rows) {
        uint16_t h = rows - y < band_rows ? rows - y : band_rows;
        const uint8_t *band = img + (size_t)y * PRINTER_WIDTH_BYTES;
        size_t len = (size_t)h * PRINTER_WIDTH_BYTES;
        // GS v 0: "print a raster image". Then width in bytes and height in rows, low byte first.
        const uint8_t head[] = { 0x1D, 'v', '0', 0,
                                 PRINTER_WIDTH_BYTES & 0xFF, PRINTER_WIDTH_BYTES >> 8,
                                 h & 0xFF, h >> 8 };
        send(head, sizeof(head));
        send(band, len);
        if (us_per_dot > 0) {
            uint32_t black = 0;
            for (size_t i = 0; i < len; i++) {
                black += (uint32_t)__builtin_popcount(band[i]);
            }
            owed_us += (int64_t)black * us_per_dot;
            if (owed_us >= tick_us) {
                uart_wait_tx_done(PRINTER_UART, portMAX_DELAY);   // the wait starts once the rows have really left
                vTaskDelay((TickType_t)(owed_us / tick_us));
                owed_us %= tick_us;
            }
        }
    }
}

static void send_image(const uint8_t *img, uint16_t rows)
{
    send_image_paced(img, rows, BAND_ROWS, PACE_US_PER_DOT);
}

/* Blank paper, sent as an all-white image so it is fed exactly like picture rows. */
static void send_footer(void)
{
    static const uint8_t white[FOOTER_ROWS * PRINTER_WIDTH_BYTES];   // all zero = no black dots
    send_image(white, FOOTER_ROWS);
}

static void finish_ticket(void)
{
    const char feed[] = { 0x1B, 'd', FEED_LINES_AFTER };   // ESC d n: feed n lines
    send(feed, sizeof(feed));
#if PRINTER_HAS_CUTTER
    CMD("\x1DV\x01");                                      // GS V 1: partial cut
#endif
}

/*
 * The header line of a ticket printed in the built-in font:
 *   TikPrint | Los Angeles  Nov 20, 2026 | 10:05 PM PST
 * Which parts appear is chosen in Settings (settings.c). Date and time come from the
 * board's clock and are left out while it isn't set. (Tickets from the web page carry
 * the same header inside their image, drawn by the page.)
 */
static void send_header(void)
{
    settings_t s;
    clock_status_t c;
    char date[24], time[24];
    settings_get(&s);
    clock_get_status(&c);
    // No location typed in: use the city of the timezone ("America/Los_Angeles" -> "Los Angeles")
    char city[CLOCK_TZ_MAX] = "";
    const char *slash = strrchr(c.timezone, '/');
    if (slash) {
        strlcpy(city, slash + 1, sizeof(city));
        for (char *p = city; *p; p++) {
            *p = *p == '_' ? ' ' : *p;
        }
    }
    const char *location = s.location[0] ? s.location : city;
    bool has_date = s.show_date && clock_format_now(date, sizeof(date), HEADER_DATE_FORMAT);
    bool has_time = s.show_time && clock_format_now(time, sizeof(time), HEADER_TIME_FORMAT);

    send_text("TikPrint");
    if (s.show_location && location[0]) {
        send_text(" | ");
        send_text(location);
    }
    if (has_date || has_time) {
        send_text("  ");
        send_text(has_date ? date : time);
        if (has_date && has_time) {
            send_text(" | ");
            send_text(time);
        }
    }
    send_text("\n");
}

static void send_job(const printer_job_t *job)
{
    CMD("\x1B@");        // ESC @: reset the printer to its defaults
    CMD("\x1Bt\x00");    // ESC t 0: code page 437 (matches send_text above)

    // Normally the page has drawn the text into the image, in the font picked there.
    // The printer's built-in font is only used when it hasn't (e.g. a ticket sent with curl).
    if (!job->text_in_image) {
        send_header();
        if (job->source[0]) {   // the ticket number is not printed
            send_text(job->source);
            send_text("\n");
        }

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
        send_footer();
    }
    finish_ticket();
}

/* Print the test line. (It used to listen on the board's own TX pin as a "pin check". The
 * receive side now listens to the printer instead: see /api/diag/printer-status.) */
static void send_test_line(const char *text)
{
    CMD("\x1B@");
    send_text(text);
    CMD("\n\n");   // no cut, so trying several settings in a row wastes little paper
    uart_wait_tx_done(PRINTER_UART, portMAX_DELAY);
}

/* ---------- Status ---------- */

/* Ask question n (1..4) and return the one-byte answer, or -1 if none came in time. */
static int ask_status(uint8_t n)
{
    const uint8_t question[] = { 0x10, 0x04, n };   // DLE EOT n
    uint8_t answer;
    uart_flush_input(PRINTER_UART);                 // drop anything old, so the next byte is the answer
    send(question, sizeof(question));
    uart_wait_tx_done(PRINTER_UART, pdMS_TO_TICKS(STATUS_REPLY_TIMEOUT_MS));
    int got = uart_read_bytes(PRINTER_UART, &answer, 1, pdMS_TO_TICKS(STATUS_REPLY_TIMEOUT_MS));
    return got == 1 ? answer : -1;
}

/* Answer to question n as bits, or 0 if it didn't answer (0 = "no problem reported"). */
static uint8_t bits_of(const printer_status_report_t *r, int n)
{
    return r->valid[n - 1] ? (uint8_t)r->raw[n - 1] : 0;
}

/* Turn the four raw answers into plain facts and one summary. */
static void decode_status(printer_status_report_t *r)
{
    r->answered = 0;
    for (int i = 0; i < PRINTER_STATUS_QUERIES; i++) {
        r->valid[i] = r->raw[i] >= 0 && (r->raw[i] & STATUS_FIXED_MASK) == STATUS_FIXED_VALUE;
        r->answered += r->valid[i] ? 1 : 0;
    }
    uint8_t s1 = bits_of(r, 1), s2 = bits_of(r, 2), s3 = bits_of(r, 3), s4 = bits_of(r, 4);
    r->online       = r->answered > 0 && !(s1 & ST1_OFFLINE);
    r->cover_open   = s2 & ST2_COVER_OPEN;
    r->paper_out    = (s4 & ST4_PAPER_END) || (s2 & ST2_PAPER_END_STOP);
    r->paper_low    = !r->paper_out && (s4 & ST4_PAPER_NEAR_END);
    r->cutter_error = s3 & ST3_CUTTER_ERROR;
    // "an error occurred" with no detail from question 3 still counts as an error
    r->other_error  = (s3 & ST3_OTHER_ERRORS) || ((s2 & ST2_ERROR) && !(s3 & ST3_CUTTER_ERROR));

    r->status = r->answered == 0                    ? PRINTER_STATUS_UNKNOWN        // silent: no wire, or printer off
              : r->cover_open                       ? PRINTER_STATUS_COVER_OPEN     // (an open cover often reads as "no paper" too)
              : r->paper_out                        ? PRINTER_STATUS_OUT_OF_PAPER
              : r->cutter_error || r->other_error   ? PRINTER_STATUS_ERROR
              : !r->online                          ? PRINTER_STATUS_OFFLINE
              : r->paper_low                        ? PRINTER_STATUS_PAPER_LOW
              :                                       PRINTER_STATUS_OK;
}

static esp_err_t thermal_read_status(printer_status_report_t *out, uint32_t wait_ms)
{
    TickType_t wait = wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
    if (xSemaphoreTake(s_uart_lock, wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;   // a ticket is being sent
    }
    memset(out, 0, sizeof(*out));
    for (int n = 1; n <= PRINTER_STATUS_QUERIES; n++) {
        out->raw[n - 1] = ask_status(n);
    }
    xSemaphoreGive(s_uart_lock);
    decode_status(out);
    return ESP_OK;
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
    s_uart_lock = xSemaphoreCreateMutex();
    if (!s_uart_lock) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(uart_driver_install(PRINTER_UART, STATUS_RX_BUFFER, 0, 0, NULL, 0), TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(PRINTER_UART, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(PRINTER_UART, PRINTER_TX_GPIO, PRINTER_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    // Pull the receive pin up, so with no wire on it the line rests quietly instead of picking up noise.
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(PRINTER_RX_GPIO, GPIO_PULLUP_ONLY), TAG, "rx pull-up");
    ESP_RETURN_ON_ERROR(printer_fake_driver.init(), TAG, "last-ticket store");
    ESP_LOGI(TAG, "printer: sending on GPIO%d, listening on GPIO%d, %d baud",
             PRINTER_TX_GPIO, PRINTER_RX_GPIO, PRINTER_BAUD);
    return ESP_OK;
}

static esp_err_t thermal_print(const printer_job_t *job)
{
    // The fake driver logs the ticket and keeps a copy of it for /api/last-ticket.
    // That copy is made now, so "last ticket" is the last one that STARTED printing.
    printer_fake_driver.print(job);
    ESP_LOGI(TAG, "sending ticket #%04u to the printer", (unsigned)job->number);
    xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    send_job(job);
    uart_wait_tx_done(PRINTER_UART, portMAX_DELAY);
    xSemaphoreGive(s_uart_lock);
    ESP_LOGI(TAG, "done");
    return ESP_OK;
}

esp_err_t printer_thermal_test(int baud, int pin, int invert)
{
    static int s_pin = PRINTER_TX_GPIO, s_invert = 0;
    char line[TEST_LINE_MAX];
    if (pin == PRINTER_RX_GPIO) {
        return ESP_ERR_INVALID_ARG;   // that pin is the one listening to the printer
    }
    xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    if (baud > 0) {
        uart_set_baudrate(PRINTER_UART, baud);
    } else {
        uint32_t now = 0;
        uart_get_baudrate(PRINTER_UART, &now);
        baud = (int)now;
    }
    if (pin >= 0 && pin != s_pin) {
        uart_set_pin(PRINTER_UART, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        s_pin = pin;
    }
    if (invert >= 0) {
        // Inverted = the wire rests at 0 V instead of 3.3 V. RS-232 printers expect that.
        uart_set_line_inverse(PRINTER_UART, invert ? (UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV) : UART_SIGNAL_INV_DISABLE);
        s_invert = invert ? 1 : 0;
    }
    snprintf(line, sizeof(line), "TikPrint test: %d baud, pin %d%s\n",
             baud, s_pin, s_invert ? ", inverted" : "");
    ESP_LOGI(TAG, "test print: %d baud, GPIO%d%s", baud, s_pin, s_invert ? ", inverted" : "");
    send_test_line(line);
    xSemaphoreGive(s_uart_lock);
    return ESP_OK;
}

/* ---------- Pace test ----------
 * Prints one strip with the same block several times, each sent a different way (rows per
 * command, pause per black dot), with a line of text above each block saying which.
 * Every block has the same number of rows, so on paper they should all be the same height.
 * A block that comes out shorter was squashed; the settings of the first full-height block
 * are the ones to use for BAND_ROWS and PACE_US_PER_DOT. */
esp_err_t printer_thermal_pace_test(const printer_pace_test_t *t)
{
    if (t->count < 1 || t->count > PRINTER_PACE_VARIANTS || t->rows < 8 || t->rows > PACE_TEST_MAX_ROWS ||
        t->black_percent < 0 || t->black_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *block = calloc((size_t)t->rows, PRINTER_WIDTH_BYTES);
    if (!block) {
        return ESP_ERR_NO_MEM;
    }
    // A fine, even pattern with the wanted share of black dots (100 = solid black).
    for (int y = 0; y < t->rows; y++) {
        for (int x = 0; x < PRINTER_WIDTH_DOTS; x++) {
            if (((x * 37 + y * 61) % 100) < t->black_percent) {
                block[y * PRINTER_WIDTH_BYTES + x / 8] |= 0x80 >> (x % 8);
            }
        }
    }
    xSemaphoreTake(s_uart_lock, portMAX_DELAY);
    CMD("\x1B@");
    for (int i = 0; i < t->count; i++) {
        int band = t->band_rows[i] < 1 ? 1 : t->band_rows[i];
        int us = t->us_per_dot[i] < 0 ? 0 : t->us_per_dot[i];
        char label[TEST_LINE_MAX];
        snprintf(label, sizeof(label), "%c: %d rows per command, %d us per dot\n", 'A' + i, band, us);
        ESP_LOGI(TAG, "pace test %s", label);
        send_text(label);
        send_image_paced(block, (uint16_t)t->rows, band, us);
        // Let this block finish and the printer settle before the next, so each one starts fresh.
        uart_wait_tx_done(PRINTER_UART, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    finish_ticket();
    uart_wait_tx_done(PRINTER_UART, portMAX_DELAY);
    xSemaphoreGive(s_uart_lock);
    free(block);
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
    .read_status = thermal_read_status,
};
