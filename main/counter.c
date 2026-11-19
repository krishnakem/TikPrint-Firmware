/*
 * Ticket counter, stored in NVS (namespace "ticket", key "count").
 */
#include "nvs.h"

#include "counter.h"

#define NVS_NS "ticket"

uint32_t counter_get(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "count", &v);
        nvs_close(h);
    }
    return v;
}

uint32_t counter_increment(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "count", &v);
        v++;
        nvs_set_u32(h, "count", v);
        nvs_commit(h);
        nvs_close(h);
    }
    return v;
}
