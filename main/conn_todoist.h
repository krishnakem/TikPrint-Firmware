#pragma once

#include <stdint.h>

#include "connectors.h"

/**
 * Turn Todoist's answer (the text of GET /api/v1/tasks/filter) into the list for a ticket:
 * overdue tasks first, then today's, the most urgent first within each.
 * now = the time in seconds, as time(NULL) gives it: decides what counts as overdue.
 * Apart from the clock's timezone it touches nothing, so it can be tried on a computer (tools/host_test).
 */
conn_result_t conn_todoist_parse(const char *json, int64_t now, conn_list_t *out);
