#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "printer.h"

/** Register all /api/... handlers on the HTTP server. */
void api_register(httpd_handle_t server);

#define API_PRINT_BODY_MAX (4 + 32 * 1024 + PRINTER_MAX_ROWS * PRINTER_WIDTH_BYTES)   // the largest print body: 622,596 bytes

/**
 * Put one ticket in the print queue from its "print body": the bytes the page POSTs to /api/print
 * (laid out above print_post in api.c). Every ticket that arrives as a print body comes through
 * here, whether the page sent it or it was downloaded from TikPrint Cloud, so both print alike.
 * The queue takes its own copy: body can be freed as soon as this returns.
 * done (may be NULL) is called with ctx when the ticket has printed or failed.
 *   ESP_OK                 queued; *ticket holds its number and its place in line
 *   ESP_ERR_INVALID_ARG    the body is not a ticket: it can never print
 *   ESP_ERR_INVALID_STATE  the queue is full: try again later
 *   ESP_ERR_NO_MEM         out of memory: try again later
 * On an error, *error is a short code ("bad_image", "queue_full", ...) and *message says it for people.
 */
esp_err_t api_print_body(const uint8_t *body, size_t len, printer_done_fn done, void *ctx,
                         printer_ticket_t *ticket, const char **error, const char **message);
