#pragma once
/* Stand-in: log lines are dropped. */
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOG_WARN 2
#define esp_log_level_set(tag, level) ((void)0)
