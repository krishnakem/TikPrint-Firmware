/*
 * Connector: Todoist. Prints the tasks due today plus the overdue ones.
 *
 * The user pastes in their API token (Todoist > Settings > Integrations > Developer).
 * One request does it all:
 *   GET https://api.todoist.com/api/v1/tasks/filter?query=today | overdue      "Authorization: Bearer <token>"
 *   -> {"results":[{"content":"Call the dentist","checked":false,"priority":4,
 *                   "due":{"date":"2026-11-22","timezone":null,...}}, ...],"next_cursor":null}
 * "priority" runs from 1 (normal) to 4 (urgent). "due.date" is a day ("2026-11-22"), a day and time
 * on the user's own clock ("2026-11-22T15:30:00"), or a moment in UTC ("2026-11-22T20:30:00Z").
 * API reference: https://developer.todoist.com/api/v1/ (checked November 2026).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"

#include "conn_http.h"
#include "conn_ticket.h"
#include "conn_todoist.h"

#define URL        "https://api.todoist.com/api/v1/tasks/filter?query=today%20%7C%20overdue&limit=50"
#define MAX_BYTES  (256 * 1024)   // 50 tasks are some 50 KB; long descriptions can make them more
#define MAX_TASKS  50             // as many as one request brings

typedef struct {
    const char *content;
    bool overdue;
    int priority;       // 1 (normal) .. 4 (urgent)
    char when[20];      // "2026-11-22T15:30", local time, or "2026-11-22~" for a day with no time: for putting them in order
    int place;          // its place in Todoist's answer
    char note[CONN_ITEM_NOTE_MAX];
} task_t;

/* Days from 1970-01-01 to a date (the usual "days from civil" sum). */
static int64_t days_from_date(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400), doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + (yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
}

/* Read a task's "due.date" into t->when, as local time. False if it is not a date. */
static bool read_due(const char *date, task_t *t, bool *has_time)
{
    int y, mo, d, h = 0, mi = 0, s = 0;
    int got = sscanf(date, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s);
    if (got < 3) {
        return false;
    }
    *has_time = got >= 5;
    if (*has_time && date[strlen(date) - 1] == 'Z') {
        // A moment in UTC: turn it into the time on the board's clock.
        time_t utc = (time_t)(days_from_date(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
        struct tm tm;
        localtime_r(&utc, &tm);
        y = tm.tm_year + 1900, mo = tm.tm_mon + 1, d = tm.tm_mday, h = tm.tm_hour, mi = tm.tm_min;
    }
    if (*has_time) {
        snprintf(t->when, sizeof(t->when), "%04d-%02d-%02dT%02d:%02d", y, mo, d, h, mi);
        snprintf(t->note, sizeof(t->note), "%d:%02d %s", h % 12 ? h % 12 : 12, mi, h < 12 ? "AM" : "PM");
    } else {
        snprintf(t->when, sizeof(t->when), "%04d-%02d-%02d~", y, mo, d);   // "~" sorts after any time of that day
    }
    return true;
}

/* Overdue first, then the most urgent, then the earliest (tasks with a time before those without), then as Todoist had them. */
static int by_order(const void *a, const void *b)
{
    const task_t *x = a, *y = b;
    if (x->overdue != y->overdue) {
        return x->overdue ? -1 : 1;
    }
    if (x->priority != y->priority) {
        return y->priority - x->priority;
    }
    int when = strcmp(x->when, y->when);
    return when ? when : x->place - y->place;
}

conn_result_t conn_todoist_parse(const char *json, int64_t now, conn_list_t *out)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *results = cJSON_GetObjectItem(root, "results");
    task_t *tasks = calloc(MAX_TASKS, sizeof(task_t));
    if (!cJSON_IsArray(results) || !tasks) {
        cJSON_Delete(root);
        free(tasks);
        return CONN_BAD_ANSWER;
    }
    char today[12];
    time_t t = (time_t)now;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(today, sizeof(today), "%Y-%m-%d", &tm);

    int count = 0;
    const cJSON *task;
    cJSON_ArrayForEach(task, results) {
        const cJSON *content = cJSON_GetObjectItem(task, "content");
        const cJSON *date = cJSON_GetObjectItem(cJSON_GetObjectItem(task, "due"), "date");
        const cJSON *priority = cJSON_GetObjectItem(task, "priority");
        if (count == MAX_TASKS || !cJSON_IsString(content) || cJSON_IsTrue(cJSON_GetObjectItem(task, "checked"))) {
            continue;
        }
        task_t *k = &tasks[count];
        bool has_time = false;
        if (cJSON_IsString(date) && read_due(date->valuestring, k, &has_time)) {
            k->overdue = strncmp(k->when, today, 10) < 0;
        }
        if (k->overdue) {
            strlcpy(k->note, "OVERDUE", sizeof(k->note));
        }
        k->content = content->valuestring;
        k->place = count;
        k->priority = cJSON_IsNumber(priority) ? priority->valueint : 1;
        count++;
    }
    qsort(tasks, (size_t)count, sizeof(task_t), by_order);
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < count; i++) {
        conn_list_add(out, tasks[i].content, tasks[i].note);
    }
    free(tasks);
    cJSON_Delete(root);   // only now: the tasks pointed into it
    return CONN_OK;
}

static conn_result_t fetch(const char field[CONN_FIELDS_MAX][CONN_FIELD_MAX], conn_list_t *out)
{
    char auth[CONN_FIELD_MAX + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", field[0]);
    const conn_request_t request = { .url = URL, .auth = auth, .max_bytes = MAX_BYTES };
    char *json;
    conn_result_t res = conn_http(&request, &json);
    memset(auth, 0, sizeof(auth));
    if (res != CONN_OK) {
        return res;
    }
    res = conn_todoist_parse(json, (int64_t)time(NULL), out);
    free(json);
    return res;
}

const connector_t conn_todoist = {
    .id = "todoist",
    .source = "TODOIST \xC2\xB7 TODAY",
    .one = "task",
    .many = "tasks",
    .nothing = "Nothing due today",
    .fields = 1,
    .fetch = fetch,
};
