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

#include "esp_log.h"
#include "esp_check.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "printer.h"
#include "clock.h"
#include "settings.h"
#include "ticket_header.h"

static const char *TAG = "thermal_printer";

/* ---- Settings you may need to change for your printer ---- */
#define PRINTER_UART       UART_NUM_1
#define PRINTER_TX_GPIO    17      // board pin wired to the printer's "TX" pin (see above)
#define PRINTER_RX_GPIO    18      // board pin wired to the printer's "RX" pin: the printer's answers. 3.3 V only!
#define PRINTER_BAUD       115200  // must match the printer (its self-test page shows it)
#define PRINTER_HAS_CUTTER 1       // 1 = cut the paper after each ticket
#define FEED_LINES_AFTER   4       // blank lines after the ticket, so it clears the tear bar
#define HEADER_COLS        64      // characters per line in the printer's small font (9 dots each), used for that header

/* Image rows sent per command. Small bands keep the printer's memory from overflowing. */
#define BAND_ROWS 64

/* Blank rows printed after a ticket's image: a bottom margin about as tall as the top one.
 * (The top margin comes for free: it is the paper between the print head and the cutter.) */
#define FOOTER_ROWS 64

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
static void send_text_n(const char *s, size_t len)
{
    char out[96];
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s, *end = p + len;
    while (p < end) {
        uint32_t cp;
        int extra;
        if (*p < 0x80)                { cp = *p;        extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else                          { cp = '?';       extra = 0; }
        p++;
        while (extra-- > 0 && p < end && (*p & 0xC0) == 0x80) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }

        const char *rep = NULL;
        char one[2] = { '?', 0 };
        if (cp < 0x80) {
            one[0] = (char)cp;
        } else {
            switch (cp) {
            // This printer reads any byte above 127 plus the byte after it as ONE Chinese character
            // (and no command tried switches that off), so only plain ASCII is ever sent:
            case 0x00B0: rep = ""; break;                       // degree sign: left out ("54F")
            case 0x00B7: one[0] = '-'; break;                   // middle dot
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

static void send_text(const char *s)
{
    send_text_n(s, strlen(s));
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
 * Which parts appear is chosen in Settings (settings.c); with_location = false leaves the
 * location out anyway (weather tickets). Date and time come from the
 * board's clock and are left out while it isn't set. (Tickets from the web page carry
 * the same header inside their image, drawn by the page, or, when the header's font there
 * is "Receipt", as a small-font line of their layout: see headerLine in web/index.html.)
 */
static void send_header(bool with_location)
{
    // Left half and right half, as on tickets drawn by the page:
    //   TikPrint | Ann Arbor, MI                      Nov 20, 2026 | 10:05 PM EST
    char left[TICKET_HEADER_MAX], right[TICKET_HEADER_MAX];
    ticket_header_text(left, right, with_location);
    // Count characters, not bytes: an accented letter is stored as two bytes but prints as one.
    int used = 0;
    for (const char *p = left; *p; p++) {
        used += ((unsigned char)*p & 0xC0) != 0x80;
    }
    int gap = HEADER_COLS - used - (int)strlen(right);
    if (gap < 2) {
        gap = 2;   // too long for one line: the printer wraps it
    }

    // The small font fits the whole header on one line, pushed out to both edges.
    CMD("\x1BM\x01");   // ESC M 1: small font (9 dots wide instead of 12)
    send_text(left);
    if (right[0]) {
        char spaces[HEADER_COLS + 1];
        memset(spaces, ' ', sizeof(spaces));
        spaces[gap < HEADER_COLS ? gap : HEADER_COLS] = 0;
        send_text(spaces);
        send_text(right);
    }
    send_text("\n");
    CMD("\x1BM\x00");   // ESC M 0: back to the normal font
}

/*
 * A ticket laid out by the page: image rows and lines in the built-in font, in turn
 * (the bytes are described under "Layout" in printer.h).
 * A line of text is set up with three commands, then sent like any other text:
 *   GS ! n    size: n = 0x00 normal, 0x11 twice as wide and tall, 0x22 three times, ...
 *   ESC E n   bold on (1) or off (0)
 *   ESC - n   underline off (0), thin (1) or thick (2)
 */
static void send_layout(const printer_job_t *job)
{
    const uint8_t *p = job->layout, *end = p + job->layout_len;
    uint16_t row = 0;   // image rows printed so far
    while (p < end) {
        if (*p == LAYOUT_ROWS) {
            uint16_t rows = p[1] | (p[2] << 8);
            send_image(job->image + (size_t)row * PRINTER_WIDTH_BYTES, rows);
            row += rows;
            p += 3;
            continue;
        }
        uint8_t size = p[1], runs = p[2];   // LAYOUT_LINE
        p += 3;
        bool small = size == LAYOUT_SMALL;  // the ticket header: normal size, but in the small font
        if (small) {
            size = 1;
            CMD("\x1BM\x01");               // ESC M 1: small font (9 dots wide instead of 12)
        }
        const char set_size[] = { 0x1D, '!', (char)(((size - 1) << 4) | (size - 1)) };
        send(set_size, sizeof(set_size));
        for (uint8_t i = 0; i < runs; i++) {
            uint8_t style = p[0], len = p[1];
            const char set_style[] = { 0x1B, 'E', (style & LAYOUT_BOLD) ? 1 : 0,
                                       0x1B, '-', (style & LAYOUT_UNDERLINE) ? (size > 1 ? 2 : 1) : 0 };
            send(set_style, sizeof(set_style));
            send_text_n((const char *)p + 2, len);
            p += 2 + len;
        }
        if (runs == 0) {
            send_text(" ");   // an empty line: the space makes the printer feed a line of this text size
        }
        send_text("\n");
        if (small) {
            CMD("\x1BM\x00");               // ESC M 0: back to the normal font
        }
    }
    CMD("\x1D!\x00" "\x1B" "E\x00" "\x1B-\x00");   // back to normal size, no bold, no underline
    if (job->image && row < job->image_rows) {         // rows no part asked for: the file under the text
        send_image(job->image + (size_t)row * PRINTER_WIDTH_BYTES, job->image_rows - row);
    }
    send_footer();
}

static void send_job(const printer_job_t *job)
{
    CMD("\x1B@");        // ESC @: reset the printer to its defaults
    CMD("\x1Bt\x00");    // ESC t 0: code page 437 (matches send_text above)

    if (job->layout && job->layout_len) {
        send_layout(job);
        finish_ticket();
        return;
    }

    // Normally the page has drawn the text into the image, in the font picked there.
    // The printer's built-in font is only used when it hasn't (e.g. a ticket sent with curl),
    // or for lines set in the page's "Receipt" font (send_layout above).
    bool image_on_top = job->image && job->image_rows && job->image_first && !job->text_in_image;
    if (!job->text_in_image) {
        send_header(!job->no_location);
        if (job->source[0]) {   // the ticket number is not printed
            send_text(job->source);
            send_text("\n");
        }
        if (image_on_top) {     // e.g. the weather symbol: between the source line and the title
            send_image(job->image, job->image_rows);
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
    if (job->image && job->image_rows && !image_on_top) {
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
