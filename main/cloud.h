#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * The board's side of TikPrint Cloud: pairing with an account, then checking in every few
 * seconds for tickets to print (docs/device-api.md in the TikPrint Cloud repo is the contract).
 *
 * The rule above all others: A PRINTER THAT IS NOT LINKED NEVER CALLS THE CLOUD.
 * The first call ever made is "pair/start", and only because someone pressed Upgrade on the
 * printer's own page (cloud_pair). There is not even a task until then.
 *
 * Once linked, the board checks in by itself for as long as it has a device token. The moment
 * the cloud says the token is no longer valid, the token is deleted and the board is local only again.
 */

#define CLOUD_CODE_MAX 16   // room for a pairing code such as "K7QM-3XPD", including the ending 0

typedef enum {
    CLOUD_LOCAL,      // not linked, nothing going on: the board calls nobody
    CLOUD_STARTING,   // Upgrade was pressed: asking the cloud for a pairing code
    CLOUD_PAIRING,    // the ticket with the QR code is out: waiting for the owner to scan it and confirm
    CLOUD_LINKED,     // linked: a device token is saved
    CLOUD_EXPIRED,    // nobody confirmed the code in time: press Upgrade again
    CLOUD_FAILED,     // the code could not be got, or the cloud refused: `message` says why
} cloud_phase_t;

typedef struct {
    cloud_phase_t phase;
    char code[CLOUD_CODE_MAX];   // while pairing: the code on the ticket, for typing in by hand. Else ""
    int expires_in_s;            // while pairing: seconds the code is still good for
    char message[64];            // CLOUD_FAILED: what went wrong, for people. Else ""
} cloud_status_t;

/** Call once at startup, after cloud_state_init(), cloud_identity_init() and printer_init().
 *  A linked printer starts checking in for jobs. One that is not linked calls nobody. */
esp_err_t cloud_init(void);

void cloud_get_status(cloud_status_t *out);

/** "local", "starting", "pairing", "linked", "expired" or "failed". */
const char *cloud_phase_name(cloud_phase_t phase);

/**
 * Upgrade was pressed: ask the cloud for a pairing code, print the ticket and wait to be linked.
 * Returns at once; how it goes shows up in cloud_get_status().
 *   ESP_ERR_INVALID_STATE  the printer is linked already
 *   ESP_ERR_NO_MEM         out of memory
 */
esp_err_t cloud_pair(void);

/**
 * "Reset / Unlink" was pressed on the printer's own page. The token is deleted here and now,
 * whatever happens next: from this moment the printer is local only. The cloud is then told in
 * the background, if it can be reached (if not, the website lists the printer until the owner
 * removes it there). Also stops a pairing that is under way.
 */
void cloud_unlink(void);
