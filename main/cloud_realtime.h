#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Listening on Supabase Realtime, so that a print sent from anywhere comes out in about a second
 * (docs/device-api.md in the TikPrint Cloud repo, section "Realtime").
 *
 * A linked printer keeps one WebSocket open (a connection that stays open, on which the other side
 * can speak at any time) and hears each new command meant for it: "print this job" or "update to
 * version X". Checking in for jobs (cloud.c) stays as the backup: every 15 minutes while the
 * socket is up, and as often as before while it is not.
 *
 * Who does what:
 *   cloud.c           everything that uses the DEVICE TOKEN: checking in, downloading and reporting
 *                     jobs, and asking the cloud for a Realtime "session" (the address to connect
 *                     to and a short-lived access token for it).
 *   cloud_realtime.c  the socket, with that session: connecting, joining, the heartbeats, and
 *                     writing the printer's own row ("I am here, running version X").
 *                     A print it hears is handed to cloud.c (cloud_print_command), which downloads
 *                     and prints it exactly as it does a job found at a check-in.
 *   cloud_update.c    the "update" command.
 *
 * This task only exists while the printer is linked. A printer that is not linked has no socket
 * and calls nobody. Nothing of a session is ever saved to flash, and no token or key is ever logged
 * or shown in /api/cloud/status.
 */

/* What the cloud hands out at POST /api/device/realtime. Sizes include the ending 0. */
typedef struct {
    char supabase_url[128];    // "https://<ref>.supabase.co": where the printer's own row is written
    char realtime_url[192];    // "wss://<ref>.supabase.co/realtime/v1/websocket": where the socket goes
    char apikey[512];          // the site's public key
    char access_token[1100];   // this printer's own token for Realtime, good for expires_in_s. Never log it
    char printer_id[64];
    char topic[128];           // "realtime:printer-<printer id>"
    char table[64];            // "printer_commands"
    char filter[128];          // "printer_id=eq.<printer id>"
    int expires_in_s;
    int heartbeat_s;           // write the printer's row this often while listening
} cloud_realtime_session_t;

typedef enum {
    CLOUD_REALTIME_SESSION_OK,       // here is a session
    CLOUD_REALTIME_SESSION_OFF,      // 503 realtime_off: this site has no Realtime for printers. Ask again in an hour
    CLOUD_REALTIME_SESSION_FAILED,   // no answer, or one that cannot be used: try again later
} cloud_realtime_result_t;

typedef struct {
    bool running;            // the task exists (the printer is linked)
    bool listening;          // the socket is up and has been told "you are subscribed"
    const char *state;       // "stopped", "waiting", "asking", "connecting", "joining", "listening" or "off"
    char last_event[16];     // the newest thing that happened: "listening", "print", "update", "closed", ... "" if nothing yet
    int last_event_s_ago;
    unsigned commands;       // commands heard on the socket since the board started
    unsigned reconnects;     // times a socket that was listening was lost since the board started
} cloud_realtime_status_t;

/** Call once at startup. Starts nothing. */
esp_err_t cloud_realtime_init(void);

/** The printer is linked: start listening (does nothing if already started). Cloud task only. */
void cloud_realtime_start(void);

/** The printer is not linked (any more): close the socket, forget the session and end the task.
 *  Returns when all of that is done. Does nothing if not started. Cloud task only. */
void cloud_realtime_stop(void);

/** Does the Realtime task need a session (a first one, one for a new try, or a fresh token before
 *  the old one runs out)? The cloud task asks this, and answers with cloud_realtime_session(). */
bool cloud_realtime_wants_session(void);

/** The cloud's answer to POST /api/device/realtime. `session` is only read for ..._OK. */
void cloud_realtime_session(cloud_realtime_result_t result, const cloud_realtime_session_t *session);

/** True while the socket is up and subscribed: check-ins then say "X-Realtime: 1". Any task. */
bool cloud_realtime_is_listening(void);

/** Have the Realtime task look again now (there is an update step to report). Any task. */
void cloud_realtime_wake(void);

/** For GET /api/cloud/status. Any task. No token or key is in it. */
void cloud_realtime_get_status(cloud_realtime_status_t *out);
