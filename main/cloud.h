#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * The board's side of TikPrint Cloud: pairing with an account, then checking in every few
 * seconds for tickets to print (docs/device-api.md in the TikPrint Cloud repo is the contract).
 *
 * The rule above all others: A PRINTER THAT IS NOT LINKED NEVER CALLS THE CLOUD.
 * The first call ever made is "pair/start", and only because someone pressed the TikPrint Cloud button on the
 * printer's own page (cloud_pair). There is not even a task until then.
 *
 * Once linked, the board checks in by itself for as long as it has a device token. The moment
 * the cloud says the token is no longer valid, the token is deleted and the board is local only again.
 *
 * The second rule: LINKING AND UNLINKING NEED SOMEONE AT THE PRINTER. PAPER IS THE PROOF.
 * The printer's own page is open to everyone on the same Wi-Fi, so nothing asked of it can change
 * who owns the printer by itself:
 *   - The pairing code is only ever printed. It is never sent to the page, never put in
 *     /api/cloud/status, never kept for /api/last-ticket and never logged.
 *   - Unlinking from the page prints a ticket with a 6-digit code first (cloud_unlink_request), and
 *     happens only when that code is typed back (cloud_unlink_confirm). See paper_code.h.
 *   - Each of those tickets can be asked for once in CLOUD_TICKET_EVERY_S seconds, so nobody on the
 *     network can empty the paper roll.
 * (The owner can also unlink on the cloud website, signed in. That needs no paper.)
 */

#define CLOUD_CODE_MAX 16          // room for a pairing code such as "K7QM-3XPD", including the ending 0
#define CLOUD_TICKET_EVERY_S 30    // a pairing ticket, or an unlink ticket, at most once in this long

typedef enum {
    CLOUD_LOCAL,      // not linked, nothing going on: the board calls nobody
    CLOUD_STARTING,   // the TikPrint Cloud button was pressed: asking the cloud for a pairing code
    CLOUD_PAIRING,    // the ticket with the QR code is out: waiting for the owner to scan it and confirm
    CLOUD_LINKED,     // linked: a device token is saved
    CLOUD_EXPIRED,    // nobody confirmed the code in time: press the button again
    CLOUD_FAILED,     // the code could not be got, or the cloud refused: `message` says why
} cloud_phase_t;

typedef struct {
    cloud_phase_t phase;
    int expires_in_s;            // while pairing: seconds the code on the ticket is still good for
    char message[64];            // CLOUD_FAILED: what went wrong, for people. Else ""
    bool task_running;           // the cloud task exists: the board is pairing, or linked and checking in
} cloud_status_t;

/** Call once at startup, after cloud_state_init(), cloud_identity_init() and printer_init().
 *  A linked printer starts checking in for jobs. One that is not linked calls nobody. */
esp_err_t cloud_init(void);

void cloud_get_status(cloud_status_t *out);

/** "local", "starting", "pairing", "linked", "expired" or "failed". */
const char *cloud_phase_name(cloud_phase_t phase);

/* The pairing code itself is not in cloud_status_t, on purpose: it stays inside cloud.c and goes
 * to two places only, the ticket and the board's own "am I linked yet?" calls to the cloud. */

/**
 * The TikPrint Cloud button was pressed: ask the cloud for a pairing code, print the ticket and wait to be linked.
 * Returns at once; how it goes shows up in cloud_get_status().
 *   ESP_ERR_INVALID_STATE  the printer is linked already
 *   ESP_ERR_NOT_FINISHED   asked again too soon: *wait_s = seconds until it may be asked again
 *   ESP_ERR_NO_MEM         out of memory
 */
esp_err_t cloud_pair(int *wait_s);

/**
 * "Reset / Unlink" was pressed on the printer's own page. Nothing is unlinked yet: a ticket with a
 * 6-digit code is printed, and the code must come back through cloud_unlink_confirm().
 * A new code takes the place of the one before.
 *   ESP_OK                 the ticket is in the print queue. *expires_in_s = how long the code is good for
 *   ESP_ERR_INVALID_STATE  the printer is not linked: there is nothing to unlink
 *   ESP_ERR_NOT_FINISHED   asked again too soon: *wait_s = seconds until it may be asked again
 *   ESP_ERR_NO_MEM, ESP_FAIL  the ticket could not be printed (no memory, or the print queue is full)
 */
esp_err_t cloud_unlink_request(int *expires_in_s, int *wait_s);

/**
 * The code from the unlink ticket was typed in. If it is right, the printer is unlinked: the token
 * is deleted here and now, whatever happens next, and from this moment the printer is local only.
 * The cloud is then told in the background, if it can be reached (if not, the website lists the
 * printer until the owner removes it there).
 *   ESP_OK                 unlinked
 *   ESP_ERR_INVALID_ARG    wrong code. *tries_left = guesses left; at 0 the code has been cancelled
 *                          and a new ticket must be printed
 *   ESP_ERR_NOT_FOUND      there is no code to check against: none was printed, or it ran out
 */
esp_err_t cloud_unlink_confirm(const char *code, int *tries_left);
