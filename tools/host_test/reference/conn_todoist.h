#pragma once

/*
 * REFERENCE ONLY: this file is not part of the firmware any more (connectors run in TikPrint Cloud
 * since firmware 0.4.1). It is kept, unchanged in what it draws, because the cloud's drift test
 * compares its own tickets with this code, dot for dot (tools/host_test/run.sh ticket ...).
 */

#include <stdint.h>

#include "connectors.h"

/**
 * Turn Todoist's answer (the text of GET /api/v1/tasks/filter) into the list for a ticket:
 * overdue tasks first, then today's, the most urgent first within each.
 * now = the time in seconds, as time(NULL) gives it: decides what counts as overdue.
 * Apart from the clock's timezone it touches nothing, so it can be tried on a computer (tools/host_test).
 */
conn_result_t conn_todoist_parse(const char *json, int64_t now, conn_list_t *out);
