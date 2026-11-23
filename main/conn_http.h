#pragma once

#include <stddef.h>

#include "connectors.h"

/*
 * One HTTPS request to an app's API, for connectors.
 *
 * The app's certificate is checked against the certificates built into the firmware. The whole
 * answer is collected in the big external RAM (PSRAM), up to max_bytes: connectors keep answers
 * small by asking for few items and few fields.
 */

#define CONN_HTTP_HEADERS_MAX 3

typedef struct {
    const char *url;
    const char *body;         // NULL = a GET; otherwise a POST with this JSON text
    const char *auth;         // the whole "Authorization" value, e.g. "Bearer abc123". NULL = none
    struct {
        const char *name, *value;
    } header[CONN_HTTP_HEADERS_MAX];   // extra headers; unused ones are NULL
    size_t max_bytes;         // the biggest answer accepted
} conn_request_t;

/**
 * Send it. On CONN_OK, *reply is the answer as text: free() it when done.
 * Otherwise *reply is NULL and the result says why:
 *   no Wi-Fi -> CONN_NO_WIFI          401, 403 -> CONN_BAD_TOKEN       429 -> CONN_RATE_LIMITED
 *   400, 404 -> CONN_NOT_FOUND        an answer that is too big -> CONN_BAD_ANSWER
 *   anything else (no answer, 5xx) -> CONN_UNREACHABLE
 * Call it from the automations task only: it needs a deep stack and can take seconds.
 */
conn_result_t conn_http(const conn_request_t *request, char **reply);
