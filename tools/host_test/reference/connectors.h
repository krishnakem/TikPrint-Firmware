#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/*
 * Connectors: apps a list is fetched from and printed as a ticket (Todoist, ...).
 * Only what the ticket-drawing code needs is left here: what a list is, and what describes an app.
 *
 * REFERENCE ONLY: this file is not part of the firmware any more (connectors run in TikPrint Cloud
 * since firmware 0.4.1). It is kept, unchanged in what it draws, because the cloud's drift test
 * compares its own tickets with this code, dot for dot (tools/host_test/run.sh ticket ...).
 */

#define CONN_ID_MAX        16    // room for a connector's id ("todoist"), including the ending 0
#define CONN_FIELDS_MAX    2     // what the user types in: a token, and for some apps one more thing
#define CONN_FIELD_MAX     160   // room for one of those, including the ending 0
#define CONN_ITEMS_MAX     12    // lines on a ticket; the rest becomes "+ 5 more"
#define CONN_ITEM_TEXT_MAX 120   // room for one line's text
#define CONN_ITEM_NOTE_MAX 40    // ... and for the small note under it ("OVERDUE", a project, ...)

/** How fetching from the app went. */
typedef enum {
    CONN_OK = 0,
    CONN_NO_TOKEN,       // no connector is set up
    CONN_NO_WIFI,        // the board is not on Wi-Fi
    CONN_UNREACHABLE,    // the app did not answer, or answered with a server error
    CONN_BAD_TOKEN,      // it refused the token
    CONN_RATE_LIMITED,   // it said: too many requests, wait
    CONN_BAD_ANSWER,     // it answered, but not with something we can read
    CONN_NOT_FOUND,      // the token is fine but the thing asked for is not there (e.g. a wrong database ID)
} conn_result_t;

const char *conn_result_name(conn_result_t result);      // "ok", "no_token", "no_wifi", "unreachable", "bad_token", ...
const char *conn_result_message(conn_result_t result);   // the same, as a sentence for people

/** One line of the ticket. Both are plain text; "note" may be "". */
typedef struct {
    char text[CONN_ITEM_TEXT_MAX];
    char note[CONN_ITEM_NOTE_MAX];
} conn_item_t;

/** What a connector fetched. */
typedef struct {
    int count;                          // items filled in below
    int total;                          // how many there really are (>= count)
    conn_item_t item[CONN_ITEMS_MAX];
} conn_list_t;

/** Add a line to the list (it is cleaned for printing on the way in). Counts it even when the list is full. */
void conn_list_add(conn_list_t *list, const char *text, const char *note);

/** One app. */
typedef struct {
    const char *id;         // "todoist"
    const char *source;     // the ticket's source line: "TODOIST \xC2\xB7 TODAY"
    const char *one, *many; // what its items are called: "task", "tasks"
    const char *nothing;    // the ticket's title when the list is empty: "Nothing due today"
    int fields;             // how many things the user types in (1..CONN_FIELDS_MAX); the first is the token
    /** Fetch the list from the app. */
    conn_result_t (*fetch)(const char field[CONN_FIELDS_MAX][CONN_FIELD_MAX], conn_list_t *out);
} connector_t;
