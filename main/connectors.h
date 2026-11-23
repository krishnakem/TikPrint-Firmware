#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "settings.h"

/*
 * Connectors: apps the printer fetches a list from and prints as a ticket (Todoist, GitHub, ...).
 *
 * Each one runs on the printer itself and talks to the app directly, with a token the user
 * pastes in on the page. There is no server in between.
 *
 * The free-plan rules live here, in the firmware, not just on the page:
 *   - ONE connector at a time (weather is separate and does not count).
 *   - Up to CONNECTORS_MAX_PER_DAY scheduled prints a day. "Print now" is never limited.
 *   - Changing to a different app is a "switch": one switch per CONNECTORS_SWITCH_DAYS day(s).
 *     Setting up the very first connector is not a switch; pasting a new token for the same app
 *     never is. Disconnecting and then setting up a different app IS one.
 *   - A switch needs the clock, or there would be no telling when the last one was.
 * More than that is "Pro", which lives on the TikPrint Cloud site only. Nothing here unlocks it.
 *
 * To add an app: write conn_<app>.c with one connector_t, and add it to ALL in connectors.c
 * and to CONNECTORS in web/index.html.
 */

#define CONNECTORS_MAX_PER_DAY  3     // scheduled prints a day (counted apart from weather's)
#define CONNECTORS_SWITCH_DAYS  1     // 1 = one switch per calendar day; 7 = a week must pass, ...
#define CONNECTORS_SLOTS        SETTINGS_WEATHER_SLOTS

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
    const char *id;         // "todoist": used in the API and saved on the board
    const char *source;     // the ticket's source line: "TODOIST \xC2\xB7 TODAY"
    const char *one, *many; // what its items are called: "task", "tasks"
    const char *nothing;    // the ticket's title when the list is empty: "Nothing due today"
    int fields;             // how many things the user types in (1..CONN_FIELDS_MAX); the first is the token
    /** Fetch the list. Runs on the automations task (deep stack, may take seconds). */
    conn_result_t (*fetch)(const char field[CONN_FIELDS_MAX][CONN_FIELD_MAX], conn_list_t *out);
} connector_t;

/** Load what was saved. Call once NVS is up, before automations_start(). */
esp_err_t connectors_init(void);

/** The connector with this id, or NULL. */
const connector_t *connectors_find(const char *id);

/** The i-th connector this firmware has (0, 1, ...), or NULL after the last. */
const connector_t *connectors_at(int i);

/* ---- Setting up, switching, disconnecting ---- */

typedef enum {
    CONN_SET_OK = 0,
    CONN_SET_UNKNOWN,     // no such connector
    CONN_SET_BAD_FIELDS,  // a token is missing, too long, or not plain text
    CONN_SET_LIMIT,       // another connector is active and "switch" was not asked for  -> "connector_limit"
    CONN_SET_LOCKED,      // a switch was already made too recently                      -> "switch_locked"
    CONN_SET_NO_CLOCK,    // a switch needs the time, and the clock is not set           -> "clock_not_set"
} conn_set_result_t;

/**
 * Make `id` the active connector with these fields, following the rules at the top of this file.
 * allow_switch: the user has said yes to replacing the connector that is active now.
 * A switch erases the old connector's token.
 */
conn_set_result_t connectors_connect(const char *id, const char *const field[CONN_FIELDS_MAX], bool allow_switch);

/** Erase the active connector's token. The next different app set up counts as a switch. */
void connectors_disconnect(void);

/* ---- Schedule and look (they belong to whichever connector is active) ---- */

typedef struct {
    char font[SETTINGS_FONT_MAX];   // as for weather: "Inter", ...; "Printer" = the printer's built-in font
    uint8_t size;                   // 0 = small, 1 = medium, 2 = large
} connectors_look_t;

void connectors_get_schedule(settings_weather_schedule_t *out);
esp_err_t connectors_set_schedule(const settings_weather_schedule_t *in);   // ESP_ERR_INVALID_ARG if a time is not within a day
void connectors_get_look(connectors_look_t *out);
esp_err_t connectors_set_look(const connectors_look_t *in);                 // ESP_ERR_INVALID_ARG if the size or font is wrong

/* ---- State, for /api/status ---- */

typedef struct {
    char active[CONN_ID_MAX];               // "" = none set up
    bool field_set[CONN_FIELDS_MAX];
    char field_tail[CONN_FIELDS_MAX][5];    // the last 4 characters of each field, to recognise it by. Never more.
    bool had_one;                           // a connector has been set up before (so the next different one is a switch)
    bool clock_set;
    bool switch_locked;                     // a switch was made too recently
    int switch_in_days;                     // ... and may be made again in this many days (1 = tomorrow)
    int printed_today;                      // scheduled prints so far today
    bool busy;                              // a "print now" is being fetched
    bool ran, ok;                           // one has been tried since the board started / it joined the print queue
    char error[16];                         // "" if ok, else conn_result_name() or "queue_full", "no_memory"
    char message[96];
    uint32_t ticket;
    int s_ago;
} connectors_status_t;

void connectors_get_status(connectors_status_t *out);

/* ---- Printing: called by automations.c, on its task ---- */

/** Note that a "print now" is on its way. False if one already is. */
bool connectors_begin_manual(void);
void connectors_cancel_manual(void);

/** Fetch the active connector's list and put a ticket in the print queue. manual = asked for with "print now". */
void connectors_print(bool manual);

/** Look at the clock: is a scheduled print due? Call every few seconds. */
void connectors_schedule_check(void);
