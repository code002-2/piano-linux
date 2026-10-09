#include "input_monitor.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void test_distance(void) {
    assert(fabsf(input_monitor_distance(0, 0, 3, 4) - 5.0f) < 0.001f);
    assert(fabsf(input_monitor_distance(5, 0, 5, 7) - 7.0f) < 0.001f);
    assert(input_monitor_distance(10, 20, 10, 20) == 0.0f);
    assert(fabsf(input_monitor_distance(-1, -1, 2, 3) - 5.0f) < 0.001f);
}

static EventType classify(float ratio, int x, int y, float elapsed_ms,
                          float edge_x, float edge_y,
                          float gesture_delay_s) {
    return input_monitor_classify_release(
        ratio, 0.03f, x, y, 1080, 2400,
        edge_x, edge_y, elapsed_ms, gesture_delay_s);
}

static void test_release_classification(void) {
    /* An edge tap is still a tap; movement must exceed swipeThd. */
    assert(classify(0.01f, 10, 1200, 100.0f, 0.03f, 0.03f, 2.0f) ==
           EVT_TOUCH_UP);
    assert(classify(0.04f, 540, 1200, 100.0f, 0.03f, 0.03f, 2.0f) ==
           EVT_SWIPE);

    assert(classify(0.04f, 10, 1200, 100.0f, 0.03f, 0.03f, 2.0f) ==
           EVT_GESTURE);
    assert(classify(0.04f, 1070, 1200, 100.0f, 0.03f, 0.03f, 2.0f) ==
           EVT_GESTURE);
    assert(classify(0.04f, 540, 50, 100.0f, 0.03f, 0.03f, 2.0f) ==
           EVT_GESTURE);
    assert(classify(0.04f, 540, 2360, 100.0f, 0.03f, 0.03f, 2.0f) ==
           EVT_GESTURE);

    /* A slow edge swipe and disabled edge thresholds remain swipes. */
    assert(classify(0.04f, 10, 1200, 2001.0f, 0.03f, 0.03f, 2.0f) ==
           EVT_SWIPE);
    assert(classify(0.04f, 0, 0, 100.0f, 0.0f, 0.0f, 2.0f) ==
           EVT_SWIPE);

    /* Non-positive delay preserves the existing unlimited-time behavior. */
    assert(classify(0.04f, 10, 1200, 10000.0f, 0.03f, 0.03f, 0.0f) ==
           EVT_GESTURE);
}

int main(void) {
    test_distance();
    test_release_classification();
    puts("input monitor tests passed");
    return 0;
}
