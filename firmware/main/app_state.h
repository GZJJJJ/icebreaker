#pragma once

#include "gateway_client.h"

typedef enum {
    APP_STATE_BOOT,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_IDLE,
    APP_STATE_LISTENING,
    APP_STATE_THINKING,
    APP_STATE_SPEAKING,
    APP_STATE_INTERRUPTED,
    APP_STATE_ERROR,
    APP_STATE_PAUSED,
    APP_STATE_HOSTING,
} app_state_t;

static inline app_state_t app_resting_state(activity_state_t activity)
{
    return activity == ACTIVITY_ACTIVE ? APP_STATE_HOSTING :
           activity == ACTIVITY_PAUSED ? APP_STATE_PAUSED : APP_STATE_IDLE;
}
