#pragma once
typedef void *SemaphoreHandle_t;
#define xSemaphoreCreateMutex() ((void *)1)
#define xSemaphoreTake(lock, wait) ((void)(lock))
#define xSemaphoreGive(lock) ((void)(lock))
