#pragma once

#include "esp_err.h"

typedef enum {
    WT32_DISPLAY_CHAT_USER,
    WT32_DISPLAY_CHAT_ASSISTANT,
} wt32_display_chat_role_t;

esp_err_t wt32_display_init(void);
esp_err_t wt32_display_append_chat_message(wt32_display_chat_role_t role, const char *text);
