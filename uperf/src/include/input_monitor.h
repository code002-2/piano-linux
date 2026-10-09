#ifndef UPERF_INPUT_MONITOR_H
#define UPERF_INPUT_MONITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "state_machine.h"

/* Linux input event types we care about (subset of linux/input-event-codes.h) */
#define INPUT_EV_KEY   1
#define INPUT_EV_ABS   3
#define INPUT_BTN_TOUCH 0x14a

/* Absolute axes for touchscreen */
#define INPUT_ABS_MT_POSITION_X  0x35
#define INPUT_ABS_MT_POSITION_Y  0x36
#define INPUT_ABS_MT_PRESSURE    0x18
#define INPUT_ABS_MT_TRACKING_ID 0x39
#define INPUT_ABS_X              0x00
#define INPUT_ABS_Y              0x01

/* Maximum number of touchscreen input devices to monitor */
#define INPUT_MAX_DEVICES 8

/* Raw touch sample from an input event */
typedef struct {
    uint64_t ts_ms;      /* Timestamp in milliseconds */
    int32_t  x;
    int32_t  y;
    int32_t  pressure;
    int32_t  tracking_id;
    bool     touch_down; /* true = touch down, false = touch up */
} TouchSample;

/* Classified input event emitted to the state machine */
typedef struct {
    EventType type;  /* Maps to EventType in state_machine.h */
    float     distance_ratio;  /* Swipe distance / screen diagonal */
    float     velocity;        /* End velocity of swipe */
    int32_t   start_x;
    int32_t   start_y;
} InputEvent;

/* Opaque input monitor handle */
typedef struct InputMonitor InputMonitor;

/* Pure helpers used by the event parser and unit tests.  gesture_delay_time
 * is expressed in seconds, while elapsed_ms is in milliseconds. */
float input_monitor_distance(int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2);
EventType input_monitor_classify_release(
    float distance_ratio, float swipe_thd,
    int32_t start_x, int32_t start_y,
    int screen_width, int screen_height,
    float gesture_thd_x, float gesture_thd_y,
    float elapsed_ms, float gesture_delay_time);

/* Create a new input monitor.
 * swipe_thd, gesture_thd_x, gesture_thd_y, gesture_delay_time:
 *   thresholds from config (input module).
 * screen_width, screen_height: physical screen dimensions in pixels
 *   (for distance ratio calculation).
 * Returns NULL on failure. */
InputMonitor *input_monitor_new(float swipe_thd, float gesture_thd_x,
                                float gesture_thd_y, float gesture_delay_time,
                                int screen_width, int screen_height);

/* Free input monitor resources. Closes all device fds. */
void input_monitor_free(InputMonitor *im);

/* Discover and open all touchscreen input devices.
 * Returns the number of devices opened, or -1 on error. */
int input_monitor_discover_devices(InputMonitor *im);

/* Close all currently-opened input devices. */
void input_monitor_close_devices(InputMonitor *im);

/* Get the epoll fd for multiplexing with other event sources.
 * Returns -1 on error. */
int input_monitor_get_epoll_fd(const InputMonitor *im);

/* Poll for new input events. Must be called periodically.
 * Fills the provided InputEvent array (up to max_events).
 * Returns number of events written, or 0 if no events. */
int input_monitor_poll(InputMonitor *im, InputEvent *events, int max_events);

/* Set the screen dimensions (may change on rotate). */
void input_monitor_set_screen_size(InputMonitor *im, int w, int h);

/* Get the number of currently-opened input devices. */
int input_monitor_device_count(const InputMonitor *im);

#endif /* UPERF_INPUT_MONITOR_H */
