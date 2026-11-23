#pragma once

#include "esp_err.h"

/*
 * The two tickets that prove someone is at the printer (see cloud.h). Both are "secret" tickets
 * (printer_job_t.secret): they print like any other, but the paper is the only place their code
 * can be read. Not /api/last-ticket, not the log.
 */

/**
 * Print the pairing ticket: a QR code of link_url to scan with a phone, and the code as text
 * for typing in by hand. It joins the print queue like any other ticket.
 *   ESP_ERR_INVALID_ARG    link_url is too long for a QR code
 *   ESP_ERR_NO_MEM         out of memory
 *   ESP_ERR_INVALID_STATE  the print queue is full
 */
esp_err_t cloud_ticket_print(const char *link_url, const char *code, int minutes);

/**
 * Print the unlink ticket: the 6-digit code to type into the printer's page to confirm
 * "Reset / Unlink". It joins the print queue like any other ticket.
 *   ESP_ERR_NO_MEM         out of memory
 *   ESP_ERR_INVALID_STATE  the print queue is full
 */
esp_err_t cloud_ticket_print_unlink(const char *code, int minutes);
