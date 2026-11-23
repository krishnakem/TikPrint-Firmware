/*
 * Checks the free-plan rules in main/connectors.c on a computer (see README.txt).
 *
 * Run twice, as "before" and "after": in between the pretend board "restarts", keeping only
 * what it saved. Two pretend apps (test_a, test_b) stand beside the real ones. The clock is
 * pretend too, so a test can jump to tomorrow.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clock.h"
#include "conn_http.h"
#include "connectors.h"
#include "nvs.h"
#include "printer.h"
#include "settings.h"
#include "ticket_header.h"

/* ---------- The pretend board ---------- */

static time_t s_now;            // the pretend time
static bool s_clock_set = true;
static int s_prints;            // tickets that joined the "print queue"
static char s_last_title[64];
static int s_items = 3;         // how many items the pretend apps have
static conn_result_t s_fetch_result = CONN_OK;
static const char *s_nvs_file;

#undef time
time_t test_time(time_t *out) { if (out) { *out = s_now; } return s_now; }
int64_t esp_timer_get_time(void) { return (int64_t)s_now * 1000000; }
void clock_get_status(clock_status_t *out) { memset(out, 0, sizeof(*out)); out->set = s_clock_set; }
int32_t clock_day_number(int64_t unix_s)
{
    time_t t = (time_t)unix_s;
    struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}
void settings_get(settings_t *out) { memset(out, 0, sizeof(*out)); strcpy(out->header_font, "Inter"); }
void ticket_header_text(char *left, char *right, bool with_location) { (void)with_location; strcpy(left, "TikPrint"); right[0] = 0; }
conn_result_t conn_http(const conn_request_t *request, char **reply) { (void)request; *reply = NULL; return CONN_UNREACHABLE; }
esp_err_t printer_print_job(const printer_job_t *job, printer_ticket_t *ticket)
{
    s_prints++;
    snprintf(s_last_title, sizeof(s_last_title), "%s", job->title);
    ticket->number = (uint32_t)s_prints;
    ticket->position = 0;
    return ESP_OK;
}

static conn_result_t test_fetch(const char field[CONN_FIELDS_MAX][CONN_FIELD_MAX], conn_list_t *out)
{
    (void)field;
    for (int i = 0; i < s_items && s_fetch_result == CONN_OK; i++) {
        conn_list_add(out, "Something to do", "");
    }
    return s_fetch_result;
}
const connector_t conn_test_a = { .id = "test_a", .source = "TEST A", .one = "thing", .many = "things", .nothing = "Nothing to do", .fields = 1, .fetch = test_fetch };
const connector_t conn_test_b = { .id = "test_b", .source = "TEST B", .one = "thing", .many = "things", .nothing = "Nothing to do", .fields = 2, .fetch = test_fetch };

/* Saved settings: a small list in memory, written to a file so that it outlives a "restart". */
typedef struct { char key[40]; size_t len; unsigned char value[400]; } entry_t;
static entry_t s_nvs[32];
static const char *s_ns[8];
static void nvs_load(void) { FILE *f = fopen(s_nvs_file, "rb"); if (f) { fread(s_nvs, sizeof(s_nvs), 1, f); fclose(f); } }
static entry_t *nvs_find(nvs_handle_t h, const char *key, bool make)
{
    char full[40];
    snprintf(full, sizeof(full), "%s/%s", s_ns[h], key);
    entry_t *free_one = NULL;
    for (int i = 0; i < 32; i++) {
        if (strcmp(s_nvs[i].key, full) == 0) { return &s_nvs[i]; }
        if (!s_nvs[i].key[0] && !free_one) { free_one = &s_nvs[i]; }
    }
    if (make && free_one) { strcpy(free_one->key, full); }
    return make ? free_one : NULL;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)mode; static int n; *h = n++ % 8; s_ns[*h] = ns; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; FILE *f = fopen(s_nvs_file, "wb"); fwrite(s_nvs, sizeof(s_nvs), 1, f); fclose(f); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n) { entry_t *e = nvs_find(h, key, true); memcpy(e->value, v, n); e->len = n; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *v, size_t *n)
{
    entry_t *e = nvs_find(h, key, false);
    if (!e || e->len > *n) { return ESP_FAIL; }
    memcpy(v, e->value, e->len);
    *n = e->len;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *v) { return nvs_set_blob(h, key, v, strlen(v) + 1); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *v, size_t *n) { return nvs_get_blob(h, key, v, n); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) { entry_t *e = nvs_find(h, key, false); if (e) { memset(e, 0, sizeof(*e)); } return ESP_OK; }
static bool saved_anywhere(const char *text)
{
    return memmem(s_nvs, sizeof(s_nvs), text, strlen(text)) != NULL;
}

/* ---------- The checks ---------- */

static int s_failed, s_checked;
#define CHECK(what, ok) do { s_checked++; if (!(ok)) { s_failed++; printf("  FAILED: %s  (line %d)\n", what, __LINE__); } else { printf("  ok: %s\n", what); } } while (0)

static conn_set_result_t connect(const char *id, const char *f0, const char *f1, bool allow_switch)
{
    const char *const field[CONN_FIELDS_MAX] = { f0, f1 };
    return connectors_connect(id, field, allow_switch);
}
static connectors_status_t status(void) { connectors_status_t st; connectors_get_status(&st); return st; }
static time_t at(int y, int mo, int d, int h, int mi)
{
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_isdst = -1 };
    return mktime(&tm);
}
/* Let the schedule look at the clock every 10 seconds from now until `until`, as the board does. */
static void run_until(time_t until) { for (; s_now <= until; s_now += 10) { connectors_schedule_check(); } }

static void before_restart(void)
{
    s_now = at(2026, 11, 23, 9, 0);
    connectors_init();

    printf("Setting up\n");
    CHECK("an app the printer does not have is refused", connect("nope", "tok", NULL, false) == CONN_SET_UNKNOWN);
    CHECK("a token with a space in it is refused", connect("test_a", "abc def", NULL, false) == CONN_SET_BAD_FIELDS);
    CHECK("an app with two boxes needs both", connect("test_b", "tokenB", NULL, false) == CONN_SET_BAD_FIELDS);
    s_clock_set = false;
    CHECK("the first connector is set up, even with no clock", connect("test_a", "SECRET-TOKEN-A-1234", NULL, false) == CONN_SET_OK);
    s_clock_set = true;
    CHECK("... and that was not a switch", !status().switch_locked && strcmp(status().active, "test_a") == 0);
    CHECK("a new token for the same app is fine", connect("test_a", "SECRET-TOKEN-A-5678", NULL, false) == CONN_SET_OK && !status().switch_locked);

    printf("Tokens stay on the board\n");
    connectors_status_t st = status();
    CHECK("the status has only the last 4 characters", strcmp(st.field_tail[0], "5678") == 0 && !memmem(&st, sizeof(st), "SECRET", 6));
    CHECK("a short token gives nothing of itself away", connect("test_a", "short", NULL, false) == CONN_SET_OK && status().field_tail[0][0] == 0);
    connect("test_a", "SECRET-TOKEN-A-5678", NULL, false);

    printf("One connector at a time\n");
    CHECK("a second connector is refused (connector_limit)", connect("test_b", "tokenB", "db", false) == CONN_SET_LIMIT);
    CHECK("... and the first is still the active one", strcmp(status().active, "test_a") == 0 && saved_anywhere("SECRET-TOKEN-A-5678"));
    s_clock_set = false;
    CHECK("with no clock a switch is refused (clock_not_set)", connect("test_b", "tokenB", "db", true) == CONN_SET_NO_CLOCK);
    s_clock_set = true;
    CHECK("with \"switch\" it replaces the first", connect("test_b", "SECRET-TOKEN-B-9999", "db", true) == CONN_SET_OK && strcmp(status().active, "test_b") == 0);
    CHECK("... whose token is erased from the board", !saved_anywhere("SECRET-TOKEN-A") && saved_anywhere("SECRET-TOKEN-B-9999"));

    printf("One switch a day\n");
    CHECK("a second switch the same day is refused (switch_locked)", connect("test_a", "tokenA", NULL, true) == CONN_SET_LOCKED);
    CHECK("the page is told: locked, again tomorrow", status().switch_locked && status().switch_in_days == 1);
    connectors_disconnect();
    CHECK("disconnecting and setting up another app is a switch too", connect("test_a", "tokenA", NULL, false) == CONN_SET_LOCKED);
    CHECK("... but the same app may come back", connect("test_b", "SECRET-TOKEN-B-9999", "db", false) == CONN_SET_OK);
    s_now = at(2026, 11, 23, 23, 59);
    CHECK("still locked at 11:59 PM", connect("test_a", "tokenA", NULL, true) == CONN_SET_LOCKED);

    printf("The schedule: 3 prints a day, counted apart from \"print now\"\n");
    settings_weather_schedule_t sched = { .on = true, .slot = { { 8 * 60, true }, { 13 * 60, true }, { 18 * 60, true } } };
    connectors_set_schedule(&sched);
    s_now = at(2026, 11, 24, 7, 0);
    s_prints = 0;
    run_until(at(2026, 11, 24, 7, 59));
    CHECK("nothing prints before its time", s_prints == 0);
    run_until(at(2026, 11, 24, 8, 30));
    CHECK("the 8:00 slot prints once", s_prints == 1 && strcmp(s_last_title, "3 things") == 0);
    connectors_print(true);
    connectors_print(true);
    CHECK("\"print now\" always prints", s_prints == 3);
    s_items = 0;
    run_until(at(2026, 11, 24, 13, 30));
    CHECK("an empty list still prints a short ticket", s_prints == 4 && strcmp(s_last_title, "Nothing to do") == 0);
    s_items = 3;
    run_until(at(2026, 11, 24, 18, 30));
    CHECK("the third slot prints: 3 scheduled today", s_prints == 5 && status().printed_today == 3);
    sched.slot[0].minute = 20 * 60;   // move a slot to 8 PM to try for a fourth
    connectors_set_schedule(&sched);
    run_until(at(2026, 11, 24, 21, 0));
    CHECK("a fourth scheduled print the same day does not happen", s_prints == 5);
    connectors_print(true);
    CHECK("... while \"print now\" still does", s_prints == 6);

    printf("When the app cannot be reached\n");
    s_now = at(2026, 11, 25, 12, 59);
    s_fetch_result = CONN_BAD_TOKEN;
    s_prints = 0;
    run_until(at(2026, 11, 25, 13, 30));
    st = status();
    CHECK("a refused token prints nothing and says \"bad_token\"", s_prints == 0 && st.ran && !st.ok && strcmp(st.error, "bad_token") == 0);
    s_fetch_result = CONN_OK;

    printf("The next day\n");
    CHECK("a switch is allowed again", !status().switch_locked && connect("test_a", "SECRET-TOKEN-A-0000", NULL, true) == CONN_SET_OK);
}

static void after_restart(void)
{
    s_now = at(2026, 11, 25, 14, 0);
    nvs_load();
    connectors_init();
    printf("After a restart\n");
    connectors_status_t st = status();
    CHECK("the active connector and its token are remembered", strcmp(st.active, "test_a") == 0 && strcmp(st.field_tail[0], "0000") == 0);
    CHECK("... and so is today's switch", st.switch_locked && connect("test_b", "tokenB", "db", true) == CONN_SET_LOCKED);
    settings_weather_schedule_t sched;
    connectors_get_schedule(&sched);
    CHECK("... and the schedule", sched.slot[0].minute == 20 * 60 && sched.slot[1].minute == 13 * 60);
    s_prints = 0;
    connectors_disconnect();
    run_until(at(2026, 11, 25, 20, 30));
    CHECK("with no connector set up, nothing prints by itself", s_prints == 0);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        return 2;
    }
    s_nvs_file = argv[2];
    if (strcmp(argv[1], "before") == 0) {
        before_restart();
    } else {
        after_restart();
    }
    printf("%d checks, %d failed\n\n", s_checked, s_failed);
    return s_failed ? 1 : 0;
}
