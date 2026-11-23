/*
 * Connectors: the list of apps, which one is active, and the free-plan rules (see connectors.h).
 *
 * Saved on the board in two NVS namespaces:
 *   "conn"      which connector is active, when the last switch was, the schedule, the look,
 *               and which scheduled prints are done today
 *   "conn_sec"  what the user pasted in (tokens). Kept apart so that it can be encrypted one day
 *               without touching the rest. Nothing here is ever sent back to the page.
 *
 * The web server changes things and the automations task reads them, so the copy in memory is
 * guarded by one lock (s_lock).
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "clock.h"
#include "conn_ticket.h"
#include "connectors.h"
#include "printer.h"
#include "ticket_draw.h"

static const char *TAG = "connectors";

#define NVS_NS          "conn"
#define NVS_SECRETS     "conn_sec"
#define SLOT_WINDOW_MIN 15      // a scheduled print that cannot reach the app keeps trying this long, then is skipped
#define SLOT_RETRY_MS   60000   // ... trying this often

/* Every app this firmware has, ending with NULL. Each one is in its own file (conn_<app>.c). */
extern const connector_t conn_todoist;

#ifdef CONNECTORS_TEST_APPS   /* only when built by tools/host_test, which adds two pretend apps */
extern const connector_t conn_test_a, conn_test_b;
#else
#define CONNECTORS_TEST_APPS
#endif

static const connector_t *const ALL[] = {
    &conn_todoist,
    CONNECTORS_TEST_APPS
    NULL,
};

/* Which connector is active. Saved as one block. */
typedef struct {
    char active[CONN_ID_MAX];   // "" = none (never set up, or disconnected)
    char last[CONN_ID_MAX];     // the one that was active last: setting up a different one is a switch
    int64_t switched_at;        // when the last switch was made (Unix time), 0 = never
} state_t;

/* Which scheduled prints are done, so that none prints twice and the daily limit holds even if
 * the board restarts. Days are numbers like 20261121. */
typedef struct {
    int32_t slot_day[CONNECTORS_SLOTS];   // the day each slot last printed or was skipped
    int32_t count_day;                    // the day "count" is about
    int32_t count;                        // scheduled tickets printed that day
} fired_t;

/* How a try at a ticket ended. */
typedef enum { PRINT_OK, PRINT_TRY_LATER, PRINT_FAILED } print_result_t;

static SemaphoreHandle_t s_lock;   // guards everything below
static state_t s_state;
static char s_field[CONN_FIELDS_MAX][CONN_FIELD_MAX];
static fired_t s_fired;
/* The schedule. Until it is changed: one print, at 8:00 AM. */
static settings_weather_schedule_t s_schedule = {
    .on = true,
    .slot = { { 8 * 60, true }, { 13 * 60, false }, { 18 * 60, false } },
};
static connectors_look_t s_look = { .font = "Inter", .size = 1 };
static bool s_print_pending;       // a "print now" is in the mailbox or being worked on
static bool s_ran, s_ok;           // how the last print went
static char s_error[16], s_message[96];
static uint32_t s_ticket;
static int64_t s_done_us;

/* ---------- Words ---------- */

const char *conn_result_name(conn_result_t result)
{
    switch (result) {
    case CONN_OK:           return "ok";
    case CONN_NO_TOKEN:     return "no_token";
    case CONN_NO_WIFI:      return "no_wifi";
    case CONN_UNREACHABLE:  return "unreachable";
    case CONN_BAD_TOKEN:    return "bad_token";
    case CONN_RATE_LIMITED: return "rate_limited";
    case CONN_NOT_FOUND:    return "not_found";
    default:                return "bad_answer";
    }
}

const char *conn_result_message(conn_result_t result)
{
    switch (result) {
    case CONN_OK:           return "";
    case CONN_NO_TOKEN:     return "No connector is set up yet";
    case CONN_NO_WIFI:      return "The printer is not on Wi-Fi, so it cannot reach the app";
    case CONN_UNREACHABLE:  return "The app did not answer. Try again in a moment";
    case CONN_BAD_TOKEN:    return "The app refused the token. Paste a new one";
    case CONN_RATE_LIMITED: return "The app says: too many requests. Try again later";
    case CONN_NOT_FOUND:    return "The app could not find that. Check what you pasted in";
    default:                return "The app answered with something the printer cannot read";
    }
}

/* ---------- The list of apps ---------- */

const connector_t *connectors_at(int i)
{
    for (int k = 0; ALL[k]; k++) {
        if (k == i) {
            return ALL[k];
        }
    }
    return NULL;
}

const connector_t *connectors_find(const char *id)
{
    for (int k = 0; ALL[k]; k++) {
        if (strcmp(ALL[k]->id, id) == 0) {
            return ALL[k];
        }
    }
    return NULL;
}

/* ---------- Saving ---------- */

static void save_block(const char *key, const void *block, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save \"%s\"", key);
        return;
    }
    nvs_set_blob(h, key, block, size);
    nvs_commit(h);
    nvs_close(h);
}

static bool load_block(nvs_handle_t h, const char *key, void *block, size_t size)
{
    size_t n = size;
    return nvs_get_blob(h, key, block, &n) == ESP_OK && n == size;
}

/* Save what the user pasted in ("" erases it). */
static void save_fields(const char field[CONN_FIELDS_MAX][CONN_FIELD_MAX])
{
    nvs_handle_t h;
    if (nvs_open(NVS_SECRETS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the token");
        return;
    }
    for (int i = 0; i < CONN_FIELDS_MAX; i++) {
        char key[4] = { 'f', (char)('0' + i), 0 };
        if (field[i][0]) {
            nvs_set_str(h, key, field[i]);
        } else {
            nvs_erase_key(h, key);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

static void save_state_and_fields(void)
{
    state_t state;
    char (*field)[CONN_FIELD_MAX] = malloc(sizeof(s_field));   // too big for the web server's stack
    xSemaphoreTake(s_lock, portMAX_DELAY);
    state = s_state;
    if (field) {
        memcpy(field, s_field, sizeof(s_field));
    }
    xSemaphoreGive(s_lock);
    save_block("state", &state, sizeof(state));
    if (field) {
        save_fields(field);
        memset(field, 0, sizeof(s_field));
        free(field);
    }
}

static void fired_save(void)
{
    fired_t copy;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy = s_fired;
    xSemaphoreGive(s_lock);
    save_block("fired", &copy, sizeof(copy));
}

/* ---------- The switch rule ---------- */

/* When the next switch may be made: the start of the day CONNECTORS_SWITCH_DAYS after the last one. 0 = any time. */
static time_t switch_allowed_at(int64_t switched_at)
{
    if (switched_at == 0) {
        return 0;
    }
    time_t t = (time_t)switched_at;
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_mday += CONNECTORS_SWITCH_DAYS;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* Days until a switch may be made again (1 = tomorrow), 0 = it may be made now. */
static int switch_wait_days(int64_t switched_at)
{
    time_t free_at = switch_allowed_at(switched_at), now = time(NULL);
    return now >= free_at ? 0 : (int)((free_at - now + 86399) / 86400);
}

static bool fields_are_valid(const connector_t *c, const char *const field[CONN_FIELDS_MAX])
{
    for (int i = 0; i < c->fields; i++) {
        if (!field[i] || !field[i][0] || strlen(field[i]) >= CONN_FIELD_MAX) {
            return false;
        }
        for (const unsigned char *p = (const unsigned char *)field[i]; *p; p++) {
            if (*p <= 0x20 || *p >= 0x7F) {
                return false;   // tokens are plain letters, digits and signs: no spaces or line breaks
            }
        }
    }
    return true;
}

conn_set_result_t connectors_connect(const char *id, const char *const field[CONN_FIELDS_MAX], bool allow_switch)
{
    const connector_t *c = connectors_find(id);
    if (!c) {
        return CONN_SET_UNKNOWN;
    }
    if (!fields_are_valid(c, field)) {
        return CONN_SET_BAD_FIELDS;
    }
    clock_status_t clk;
    clock_get_status(&clk);

    conn_set_result_t result = CONN_SET_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same = strcmp(s_state.active, id) == 0;                          // a new token for the active app
    bool first = !s_state.last[0];                                        // the very first connector
    bool back = !s_state.active[0] && strcmp(s_state.last, id) == 0;      // disconnected, now the same app again
    bool is_switch = !same && !first && !back;
    if (is_switch && !clk.set) {
        result = CONN_SET_NO_CLOCK;
    } else if (is_switch && switch_wait_days(s_state.switched_at) > 0) {
        result = CONN_SET_LOCKED;
    } else if (is_switch && s_state.active[0] && !allow_switch) {
        result = CONN_SET_LIMIT;
    } else {
        if (is_switch) {
            s_state.switched_at = (int64_t)time(NULL);
            s_ran = false;   // how the old app's last print went no longer matters
        }
        memset(s_state.active, 0, sizeof(s_state.active));
        memset(s_state.last, 0, sizeof(s_state.last));
        strlcpy(s_state.active, id, sizeof(s_state.active));
        strlcpy(s_state.last, id, sizeof(s_state.last));
        memset(s_field, 0, sizeof(s_field));   // the old app's token goes
        for (int i = 0; i < c->fields; i++) {
            strlcpy(s_field[i], field[i], sizeof(s_field[i]));
        }
    }
    xSemaphoreGive(s_lock);
    if (result == CONN_SET_OK) {
        save_state_and_fields();
        ESP_LOGI(TAG, "connector \"%s\" is set up%s", id, is_switch ? " (a switch)" : "");
    }
    return result;
}

void connectors_disconnect(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_state.active, 0, sizeof(s_state.active));
    memset(s_field, 0, sizeof(s_field));
    s_ran = false;
    xSemaphoreGive(s_lock);
    save_state_and_fields();
    ESP_LOGI(TAG, "connector disconnected");
}

/* ---------- Schedule and look ---------- */

static bool schedule_is_valid(const settings_weather_schedule_t *s)
{
    for (int i = 0; i < CONNECTORS_SLOTS; i++) {
        if (s->slot[i].minute >= 24 * 60) {
            return false;
        }
    }
    return true;
}

void connectors_get_schedule(settings_weather_schedule_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_schedule;
    xSemaphoreGive(s_lock);
}

esp_err_t connectors_set_schedule(const settings_weather_schedule_t *in)
{
    if (!schedule_is_valid(in)) {
        return ESP_ERR_INVALID_ARG;
    }
    settings_weather_schedule_t clean;   // built field by field so the saved block has no stray bytes
    memset(&clean, 0, sizeof(clean));
    clean.on = in->on;
    bool moved = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CONNECTORS_SLOTS; i++) {
        clean.slot[i].minute = in->slot[i].minute;
        clean.slot[i].on = in->slot[i].on;
        if (clean.slot[i].minute != s_schedule.slot[i].minute) {
            s_fired.slot_day[i] = 0;   // a slot moved to a new time may print again today (the daily limit still holds)
            moved = true;
        }
    }
    bool changed = memcmp(&clean, &s_schedule, sizeof(clean)) != 0;
    s_schedule = clean;
    xSemaphoreGive(s_lock);
    if (changed) {
        save_block("sched", &clean, sizeof(clean));
    }
    if (moved) {
        fired_save();
    }
    return ESP_OK;
}

void connectors_get_look(connectors_look_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_look;
    xSemaphoreGive(s_lock);
}

esp_err_t connectors_set_look(const connectors_look_t *in)
{
    if (in->size > 2 || !in->font[0] || strlen(in->font) >= SETTINGS_FONT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    connectors_look_t clean;
    memset(&clean, 0, sizeof(clean));
    strlcpy(clean.font, in->font, sizeof(clean.font));
    clean.size = in->size;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = memcmp(&clean, &s_look, sizeof(clean)) != 0;
    s_look = clean;
    xSemaphoreGive(s_lock);
    if (changed) {
        save_block("look", &clean, sizeof(clean));
    }
    return ESP_OK;
}

/* ---------- State ---------- */

void connectors_get_status(connectors_status_t *out)
{
    clock_status_t clk;
    clock_get_status(&clk);
    memset(out, 0, sizeof(*out));
    out->clock_set = clk.set;
    int32_t today = clk.set ? clock_day_number(time(NULL)) : 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out->active, s_state.active, sizeof(out->active));
    for (int i = 0; i < CONN_FIELDS_MAX; i++) {
        size_t len = strlen(s_field[i]);
        out->field_set[i] = len > 0;
        if (len >= 12) {   // a short one would give too much of itself away
            strlcpy(out->field_tail[i], s_field[i] + len - 4, sizeof(out->field_tail[i]));
        }
    }
    out->had_one = s_state.last[0] != 0;
    // Without the clock nobody can tell how long ago the last switch was: it counts as locked.
    out->switch_in_days = clk.set ? switch_wait_days(s_state.switched_at) : 0;
    out->switch_locked = s_state.switched_at != 0 && (!clk.set || out->switch_in_days > 0);
    out->printed_today = (clk.set && s_fired.count_day == today) ? s_fired.count : 0;
    out->busy = s_print_pending;
    out->ran = s_ran;
    out->ok = s_ok;
    strlcpy(out->error, s_error, sizeof(out->error));
    strlcpy(out->message, s_message, sizeof(out->message));
    out->ticket = s_ticket;
    out->s_ago = s_ran ? (int)((esp_timer_get_time() - s_done_us) / 1000000) : 0;
    xSemaphoreGive(s_lock);
}

/* ---------- Printing ---------- */

bool connectors_begin_manual(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_print_pending;
    s_print_pending = true;
    xSemaphoreGive(s_lock);
    return !busy;
}

void connectors_cancel_manual(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_print_pending = false;
    xSemaphoreGive(s_lock);
}

/* Note how a print ended. manual: it was asked for with "print now". */
static void finished(bool manual, bool ok, const char *error, const char *message, uint32_t ticket)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ran = true;
    s_ok = ok;
    strlcpy(s_error, error, sizeof(s_error));
    strlcpy(s_message, message, sizeof(s_message));
    s_ticket = ticket;
    s_done_us = esp_timer_get_time();
    if (manual) {
        s_print_pending = false;
    }
    xSemaphoreGive(s_lock);
    if (ok) {
        ESP_LOGI(TAG, "connector ticket #%04u joined the print queue", (unsigned)ticket);
    } else {
        ESP_LOGW(TAG, "no connector ticket: %s (%s)", error, message);
    }
}

/* Fetch the active connector's list and put a ticket in the print queue.
 * Drawn in the chosen font; if that is "Printer" (or the drawing cannot be done), the printer's
 * built-in font prints the same list as plain text. */
static print_result_t print_ticket(bool manual)
{
    // Everything big lives in PSRAM (the big external RAM), not on this task's stack.
    typedef struct {
        char field[CONN_FIELDS_MAX][CONN_FIELD_MAX];
        conn_list_t list;
        char title[CONN_TICKET_TITLE_MAX];
        char body[CONN_TICKET_BODY_MAX];
    } work_t;
    work_t *w = heap_caps_calloc(1, sizeof(work_t), MALLOC_CAP_SPIRAM);
    if (!w) {
        finished(manual, false, "no_memory", "The printer is out of memory", 0);
        return PRINT_TRY_LATER;
    }
    char id[CONN_ID_MAX];
    connectors_look_t look;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(id, s_state.active, sizeof(id));
    memcpy(w->field, s_field, sizeof(w->field));
    look = s_look;
    xSemaphoreGive(s_lock);

    print_result_t result = PRINT_FAILED;
    const connector_t *c = connectors_find(id);
    conn_result_t res = c ? c->fetch(w->field, &w->list) : CONN_NO_TOKEN;
    memset(w->field, 0, sizeof(w->field));
    if (res != CONN_OK) {
        finished(manual, false, conn_result_name(res), conn_result_message(res), 0);
        result = (res == CONN_NO_WIFI || res == CONN_UNREACHABLE || res == CONN_RATE_LIMITED) ? PRINT_TRY_LATER : PRINT_FAILED;
    } else {
        settings_t header;
        settings_get(&header);
        int font = ticket_font_id(look.font), header_font = ticket_font_id(header.header_font);
        conn_ticket_title(w->title, sizeof(w->title), c, &w->list);
        conn_ticket_text(w->body, sizeof(w->body), &w->list);
        printer_job_t job = { .title = w->title, .body = w->body, .source = c->source };
        ticket_canvas_t canvas = { 0 };
        bool drawn = font >= 0 && ticket_canvas_init(&canvas, CONN_TICKET_MAX_ROWS) &&
                     conn_ticket_render(&canvas, font, look.size, header_font >= 0 ? header_font : font, c->source,
                                        w->title, &w->list);
        if (drawn) {
            job.image = canvas.bits;
            job.image_rows = (uint16_t)canvas.rows;
            job.text_in_image = true;   // header, title and list are all in the picture
        }
        printer_ticket_t ticket;
        if (printer_print_job(&job, &ticket) == ESP_OK) {
            finished(manual, true, "", "", ticket.number);
            result = PRINT_OK;
        } else {
            finished(manual, false, "queue_full", "The print queue is full, try again in a moment", 0);
            result = PRINT_TRY_LATER;
        }
        ticket_canvas_free(&canvas);
    }
    free(w);
    return result;
}

void connectors_print(bool manual)
{
    print_ticket(manual);
}

/* The same rules as weather's schedule (automations.c): a slot is "due" from its time until
 * SLOT_WINDOW_MIN minutes later and is tried until a ticket prints; each slot prints at most once
 * a day, and all slots together at most CONNECTORS_MAX_PER_DAY times. */
void connectors_schedule_check(void)
{
    static int64_t retry_at_us[CONNECTORS_SLOTS];   // not before this (board uptime)

    clock_status_t clk;
    clock_get_status(&clk);
    settings_weather_schedule_t sched;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sched = s_schedule;
    bool active = s_state.active[0] != 0;
    xSemaphoreGive(s_lock);
    if (!clk.set || !sched.on || !active) {
        return;
    }
    for (int i = 0; i < CONNECTORS_SLOTS; i++) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        int late_min = (tm.tm_hour * 60 + tm.tm_min - sched.slot[i].minute + 24 * 60) % (24 * 60);
        if (!sched.slot[i].on || late_min >= SLOT_WINDOW_MIN) {
            continue;
        }
        int32_t day = clock_day_number(now - late_min * 60);   // the day the slot started (matters just after midnight)
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool done = s_fired.slot_day[i] == day;
        bool limit = s_fired.count_day == day && s_fired.count >= CONNECTORS_MAX_PER_DAY;
        xSemaphoreGive(s_lock);
        if (done || limit || esp_timer_get_time() < retry_at_us[i]) {
            continue;
        }
        ESP_LOGI(TAG, "scheduled connector print %d", i + 1);
        print_result_t res = print_ticket(false);
        if (res == PRINT_TRY_LATER && late_min < SLOT_WINDOW_MIN - 1) {
            retry_at_us[i] = esp_timer_get_time() + SLOT_RETRY_MS * 1000LL;
            continue;
        }
        if (res != PRINT_OK) {
            ESP_LOGW(TAG, "scheduled connector print %d skipped for today", i + 1);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_fired.slot_day[i] = day;
        if (res == PRINT_OK) {
            if (s_fired.count_day != day) {
                s_fired.count_day = day;
                s_fired.count = 0;
            }
            s_fired.count++;
        }
        xSemaphoreGive(s_lock);
        fired_save();
    }
}

/* ---------- Start-up ---------- */

/* App answers can be long lists, and reading one makes thousands of small pieces. They go to PSRAM,
 * so the board's small fast memory stays free for Wi-Fi and the web server. */
static void *json_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

esp_err_t connectors_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        state_t state;
        fired_t fired;
        settings_weather_schedule_t sched;
        connectors_look_t look;
        if (load_block(h, "state", &state, sizeof(state))) {
            state.active[sizeof(state.active) - 1] = state.last[sizeof(state.last) - 1] = 0;
            s_state = state;
        }
        if (load_block(h, "fired", &fired, sizeof(fired))) {
            s_fired = fired;
        }
        if (load_block(h, "sched", &sched, sizeof(sched)) && schedule_is_valid(&sched)) {
            s_schedule = sched;
        }
        if (load_block(h, "look", &look, sizeof(look)) && look.size <= 2) {
            look.font[sizeof(look.font) - 1] = 0;
            s_look = look;
        }
        nvs_close(h);
    }
    if (s_state.active[0] && !connectors_find(s_state.active)) {
        s_state.active[0] = 0;   // saved by a firmware that had an app this one does not
    }
    if (s_state.active[0] && nvs_open(NVS_SECRETS, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < CONN_FIELDS_MAX; i++) {
            char key[4] = { 'f', (char)('0' + i), 0 };
            size_t n = sizeof(s_field[i]);
            if (nvs_get_str(h, key, s_field[i], &n) != ESP_OK) {
                s_field[i][0] = 0;
            }
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "connectors ready: active \"%s\"", s_state.active);
    return ESP_OK;
}
