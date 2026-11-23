#pragma once

#include <stdbool.h>
#include <stddef.h>

/*
 * One HTTPS call to TikPrint Cloud (the address is CLOUD_URL in config.h), as laid down in
 * docs/device-api.md in the TikPrint Cloud repo.
 *
 * - The site's certificate is checked against the certificates built into the firmware.
 * - Every call says which firmware is running (X-Firmware-Version).
 * - The connection is kept open between calls, so that checking in every few seconds does not
 *   mean a new secure connection (seconds of sums for the board) every time.
 * - The device secret and the device token only ever go to CLOUD_URL, and are never logged.
 *
 * Only the cloud task (cloud.c) may call these: there is one connection, and it is not shared.
 */

typedef enum {
    CLOUD_AUTH_DEVICE,   // X-Device-Id + X-Device-Secret: the pairing calls
    CLOUD_AUTH_TOKEN,    // Authorization: Bearer <device token>: everything else
} cloud_auth_t;

typedef struct {
    const char *name;     // what the call is, for /api/cloud/status ("last_call"): "pair/start", "jobs", ...
    bool post;            // false = GET
    const char *path;     // what comes after CLOUD_URL, e.g. "/api/device/jobs"
    cloud_auth_t auth;
    const char *token;    // the device token, for CLOUD_AUTH_TOKEN
    const char *json;     // the body of a POST, or NULL for none
    size_t max_bytes;     // the biggest answer accepted
} cloud_request_t;

typedef struct {
    int status;           // the HTTP status (200, 401, ...), or -1 if the site could not be reached
    char *body;           // the answer, whatever the status, with a 0 after it. In the big external RAM
    size_t len;           // bytes in body
    bool too_big;         // the answer was bigger than max_bytes: body holds only the start of it
} cloud_reply_t;

/** Make the call. Always fills in *reply; give its body back with cloud_reply_free(). */
void cloud_http(const cloud_request_t *request, cloud_reply_t *reply);

void cloud_reply_free(cloud_reply_t *reply);

/*
 * What has gone out to TikPrint Cloud since the board started, for GET /api/cloud/status. With no
 * serial cable attached, this is how to see that a printer that is not linked calls nobody (calls
 * stays 0), and what a linked one last did. No secret is kept here.
 */
typedef struct {
    unsigned calls;        // calls sent to the cloud since the board started
    char last_call[16];    // the name of the newest one, "" if there has been none
    char last_error[32];   // how the newest one failed: "timeout", "no_connection", "http_503", or the cloud's
                           // own word such as "invalid_token". "no_wifi" if it could not even be sent.
                           // "" if it went well, or there has been none
} cloud_http_seen_t;

/** This one may be called from any task. */
void cloud_http_get_seen(cloud_http_seen_t *out);

/** Close the connection (the board is going back to local only, or has nothing to say for a while). */
void cloud_http_close(void);
