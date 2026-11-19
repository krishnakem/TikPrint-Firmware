#pragma once

#include <stdint.h>

/** Number of tickets printed so far (kept in NVS, survives reboots). */
uint32_t counter_get(void);

/** Add one to the ticket counter, save it, and return the new value. */
uint32_t counter_increment(void);
