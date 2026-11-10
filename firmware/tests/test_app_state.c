#include <assert.h>
#include <stdio.h>

#include "app_state.h"

int main(void)
{
    assert(app_resting_state(ACTIVITY_NONE) == APP_STATE_IDLE);
    assert(app_resting_state(ACTIVITY_ACTIVE) == APP_STATE_HOSTING);
    assert(app_resting_state(ACTIVITY_PAUSED) == APP_STATE_PAUSED);
    assert(app_resting_state(ACTIVITY_ENDED) == APP_STATE_IDLE);
    puts("PASS: resting state preserves active/paused activities after a turn");
    return 0;
}
