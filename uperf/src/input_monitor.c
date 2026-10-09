#define _GNU_SOURCE
#include "input_monitor.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8u)
#define BIT_WORDS(max_bit) (((max_bit) / BITS_PER_LONG) + 1u)

static bool bitmap_test(const unsigned long *bits, unsigned int bit) {
    return (bits[bit / BITS_PER_LONG] &
            (1UL << (bit % BITS_PER_LONG))) != 0;
}

/* Internal input monitor state */
struct InputMonitor {
    /* Device state */
    int device_fds[INPUT_MAX_DEVICES];
    int device_max_x[INPUT_MAX_DEVICES];
    int device_max_y[INPUT_MAX_DEVICES];
    int nr_devices;

    /* Epoll infrastructure */
    int epoll_fd;
    struct epoll_event epoll_events[INPUT_MAX_DEVICES];

    /* Touch tracking state (per-device, simplified to single tracker) */
    struct {
        int32_t last_x;
        int32_t last_y;
        int32_t first_x;
        int32_t first_y;
        int32_t previous_x;
        int32_t previous_y;
        bool    touch_down;
        bool    pending_down;
        bool    pending_release;
        int     sample_count;
        uint64_t first_ts_ms;
        uint64_t last_ts_ms;
        float   total_distance;
        bool    is_swipe;
        bool    is_gesture;
    } tracker;

    /* Configuration */
    float swipe_thd;
    float gesture_thd_x;
    float gesture_thd_y;
    float gesture_delay_time;

    /* Screen dimensions */
    int screen_width;
    int screen_height;
    float screen_diagonal;
};

float input_monitor_distance(int32_t x1, int32_t y1,
                             int32_t x2, int32_t y2) {
    float dx = (float)x2 - (float)x1;
    float dy = (float)y2 - (float)y1;
    return hypotf(dx, dy);
}

EventType input_monitor_classify_release(
    float distance_ratio, float swipe_thd,
    int32_t start_x, int32_t start_y,
    int screen_width, int screen_height,
    float gesture_thd_x, float gesture_thd_y,
    float elapsed_ms, float gesture_delay_time) {
    if (distance_ratio <= swipe_thd) return EVT_TOUCH_UP;

    bool from_edge = false;
    if (screen_width > 0 && gesture_thd_x > 0.0f) {
        float left = (float)screen_width * gesture_thd_x;
        float right = (float)screen_width * (1.0f - gesture_thd_x);
        from_edge = (float)start_x <= left || (float)start_x >= right;
    }
    if (!from_edge && screen_height > 0 && gesture_thd_y > 0.0f) {
        float top = (float)screen_height * gesture_thd_y;
        float bottom = (float)screen_height * (1.0f - gesture_thd_y);
        from_edge = (float)start_y <= top || (float)start_y >= bottom;
    }

    bool within_gesture_time = gesture_delay_time <= 0.0f ||
        (elapsed_ms >= 0.0f &&
         elapsed_ms <= gesture_delay_time * 1000.0f);
    return from_edge && within_gesture_time ? EVT_GESTURE : EVT_SWIPE;
}

/* Check if a device file descriptor corresponds to a touchscreen */
static int is_touchscreen_device(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;

    /* Check for EV_KEY with BTN_TOUCH */
    unsigned long key_bits[BIT_WORDS(KEY_MAX)];
    memset(key_bits, 0, sizeof(key_bits));
    bool has_btn_touch =
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0 &&
        bitmap_test(key_bits, BTN_TOUCH);

    /* Check for EV_ABS with ABS_MT_POSITION_X */
    unsigned long abs_bits[BIT_WORDS(ABS_MAX)];
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
        close(fd);
        return 0;
    }
    bool has_mt_x = bitmap_test(abs_bits, ABS_MT_POSITION_X);
    bool has_mt_y = bitmap_test(abs_bits, ABS_MT_POSITION_Y);
    bool has_abs_x = bitmap_test(abs_bits, ABS_X);
    bool has_abs_y = bitmap_test(abs_bits, ABS_Y);

    unsigned long prop_bits[BIT_WORDS(INPUT_PROP_MAX)];
    memset(prop_bits, 0, sizeof(prop_bits));
    bool has_direct = ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits) >= 0 &&
                      bitmap_test(prop_bits, INPUT_PROP_DIRECT);

    close(fd);
    return (has_mt_x && has_mt_y && has_direct) ||
           (has_btn_touch && has_abs_x && has_abs_y && has_direct);
}

InputMonitor *input_monitor_new(float swipe_thd, float gesture_thd_x,
                                float gesture_thd_y, float gesture_delay_time,
                                int screen_width, int screen_height) {
    InputMonitor *im = calloc(1, sizeof(*im));
    if (!im) return NULL;

    im->swipe_thd = swipe_thd;
    im->gesture_thd_x = gesture_thd_x;
    im->gesture_thd_y = gesture_thd_y;
    im->gesture_delay_time = gesture_delay_time;
    im->screen_width = screen_width;
    im->screen_height = screen_height;
    im->screen_diagonal = sqrtf((float)(screen_width * screen_width +
                                screen_height * screen_height));

    /* Initialize tracker */
    memset(&im->tracker, 0, sizeof(im->tracker));
    im->tracker.last_x = -1;
    im->tracker.last_y = -1;
    im->tracker.previous_x = -1;
    im->tracker.previous_y = -1;

    /* Create epoll fd */
    im->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (im->epoll_fd < 0) {
        log_error("input_monitor: epoll_create1 failed: %s", strerror(errno));
        free(im);
        return NULL;
    }

    /* Initialize device fds to -1 */
    for (int i = 0; i < INPUT_MAX_DEVICES; i++)
        im->device_fds[i] = -1;

    log_info("InputMonitor created: screen=%dx%d swipeThd=%.3f gesture=(%.3f, %.3f)",
             screen_width, screen_height, swipe_thd, gesture_thd_x, gesture_thd_y);
    return im;
}

void input_monitor_free(InputMonitor *im) {
    if (!im) return;

    input_monitor_close_devices(im);

    if (im->epoll_fd >= 0) {
        close(im->epoll_fd);
    }

    log_debug("InputMonitor destroyed");
    free(im);
}

int input_monitor_discover_devices(InputMonitor *im) {
    if (!im) return -1;

    /* Scan /dev/input/ for event devices */
    DIR *dir = opendir("/dev/input/");
    if (!dir) {
        log_error("input_monitor: cannot open /dev/input/: %s", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    im->nr_devices = 0;

    while ((ent = readdir(dir)) != NULL && im->nr_devices < INPUT_MAX_DEVICES) {
        if (ent->d_name[0] != 'e') continue;  /* Skip non-event* names */

        char path[512];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        if (is_touchscreen_device(path)) {
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                log_warn("input_monitor: cannot open %s: %s", path, strerror(errno));
                continue;
            }

            im->device_fds[im->nr_devices] = fd;
            struct input_absinfo abs_x = {0}, abs_y = {0};
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) < 0)
                ioctl(fd, EVIOCGABS(ABS_X), &abs_x);
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) < 0)
                ioctl(fd, EVIOCGABS(ABS_Y), &abs_y);
            im->device_max_x[im->nr_devices] = abs_x.maximum > 0
                ? abs_x.maximum : im->screen_width;
            im->device_max_y[im->nr_devices] = abs_y.maximum > 0
                ? abs_y.maximum : im->screen_height;

            /* Add to epoll */
            struct epoll_event ev = {0};
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            if (epoll_ctl(im->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                log_error("input_monitor: epoll_ctl ADD fd=%d: %s", fd, strerror(errno));
                close(fd);
                im->device_fds[im->nr_devices] = -1;
                continue;
            }

            /* Get device name for logging */
            char name[64];
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            log_info("input_monitor: found touchscreen #%d: %s (%s)",
                     im->nr_devices, ent->d_name, name);
            im->nr_devices++;
        }
    }

    closedir(dir);
    log_info("input_monitor: discovered %d touchscreen device(s)", im->nr_devices);
    return im->nr_devices;
}

void input_monitor_close_devices(InputMonitor *im) {
    if (!im) return;

    for (int i = 0; i < im->nr_devices; i++) {
        if (im->device_fds[i] >= 0) {
            /* Remove from epoll */
            epoll_ctl(im->epoll_fd, EPOLL_CTL_DEL, im->device_fds[i], NULL);
            close(im->device_fds[i]);
            im->device_fds[i] = -1;
        }
    }
    im->nr_devices = 0;
}

int input_monitor_get_epoll_fd(const InputMonitor *im) {
    if (!im) return -1;
    return im->epoll_fd;
}

int input_monitor_device_count(const InputMonitor *im) {
    if (!im) return 0;
    return im->nr_devices;
}

void input_monitor_set_screen_size(InputMonitor *im, int w, int h) {
    if (!im) return;
    im->screen_width = w;
    im->screen_height = h;
    im->screen_diagonal = sqrtf((float)(w * w + h * h));
}

/* Convert timeval to milliseconds (evdev uses struct timeval, not timespec) */
static uint64_t timeval_to_ms(const struct timeval *tv) {
    return (uint64_t)tv->tv_sec * 1000 + tv->tv_usec / 1000;
}

int input_monitor_poll(InputMonitor *im, InputEvent *events, int max_events) {
    if (!im || im->nr_devices == 0) return 0;
    if (!events || max_events <= 0) return 0;

    /* Prepare epoll events array */
    struct epoll_event ep_events[INPUT_MAX_DEVICES];
    int nfds = epoll_wait(im->epoll_fd, ep_events, INPUT_MAX_DEVICES, 0);
    if (nfds < 0) {
        if (errno != EINTR)
            log_error("input_monitor: epoll_wait failed: %s", strerror(errno));
        return 0;
    }

    int event_count = 0;

    for (int e = 0; e < nfds && event_count < max_events; e++) {
        int fd = ep_events[e].data.fd;
        int device_index = -1;
        for (int index = 0; index < im->nr_devices; index++) {
            if (im->device_fds[index] == fd) {
                device_index = index;
                break;
            }
        }
        if (device_index < 0) continue;

        /* Read batch of input events from this device */
        struct input_event ie[16];
        ssize_t n = read(fd, &ie, sizeof(ie));
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                log_warn("input_monitor: read from fd %d: %s", fd, strerror(errno));
            continue;
        }

        size_t nevents = n / sizeof(struct input_event);
        for (size_t i = 0; i < nevents; i++) {
            struct input_event *ev = &ie[i];
            uint64_t ts_ms = timeval_to_ms(&ev->time);

            switch (ev->type) {
                case EV_ABS:
                    switch (ev->code) {
                        case ABS_X:
                        case ABS_MT_POSITION_X:
                            im->tracker.last_x = (int32_t)(
                                (int64_t)ev->value * im->screen_width /
                                im->device_max_x[device_index]);
                            break;

                        case ABS_Y:
                        case ABS_MT_POSITION_Y:
                            im->tracker.last_y = (int32_t)(
                                (int64_t)ev->value * im->screen_height /
                                im->device_max_y[device_index]);
                            break;

                        case ABS_MT_TRACKING_ID:
                            if (ev->value < 0)
                                im->tracker.pending_release = true;
                            else if (!im->tracker.touch_down &&
                                     !im->tracker.pending_down) {
                                im->tracker.pending_down = true;
                                im->tracker.last_x = -1;
                                im->tracker.last_y = -1;
                            }
                            break;
                    }
                    break;

                case EV_KEY:
                    switch (ev->code) {
                        case BTN_TOUCH:
                            if (ev->value > 0 && !im->tracker.touch_down &&
                                !im->tracker.pending_down)
                                im->tracker.pending_down = true;
                            else if (ev->value == 0)
                                im->tracker.pending_release = true;
                            break;
                    }
                    break;

                case EV_SYN:
                    if (ev->code != SYN_REPORT) break;

                    if (im->tracker.pending_down &&
                        im->tracker.last_x >= 0 && im->tracker.last_y >= 0) {
                        im->tracker.pending_down = false;
                        im->tracker.touch_down = true;
                        im->tracker.first_x = im->tracker.last_x;
                        im->tracker.first_y = im->tracker.last_y;
                        im->tracker.previous_x = im->tracker.last_x;
                        im->tracker.previous_y = im->tracker.last_y;
                        im->tracker.total_distance = 0.0f;
                        im->tracker.first_ts_ms = ts_ms;
                        im->tracker.sample_count = 0;

                        InputEvent *out = &events[event_count++];
                        memset(out, 0, sizeof(*out));
                        out->type = EVT_TOUCH_DOWN;
                        out->start_x = im->tracker.first_x;
                        out->start_y = im->tracker.first_y;
                        if (event_count >= max_events) goto done;
                    } else if (im->tracker.touch_down &&
                               im->tracker.last_x >= 0 &&
                               im->tracker.last_y >= 0 &&
                               im->tracker.previous_x >= 0 &&
                               im->tracker.previous_y >= 0) {
                        im->tracker.total_distance += input_monitor_distance(
                            im->tracker.previous_x, im->tracker.previous_y,
                            im->tracker.last_x, im->tracker.last_y);
                        im->tracker.previous_x = im->tracker.last_x;
                        im->tracker.previous_y = im->tracker.last_y;
                    }

                    if (im->tracker.pending_release) {
                        im->tracker.pending_release = false;
                        im->tracker.pending_down = false;
                        if (im->tracker.touch_down) {
                            im->tracker.touch_down = false;
                            float distance_ratio = im->tracker.total_distance /
                                                   im->screen_diagonal;
                            float elapsed_ms = ts_ms >= im->tracker.first_ts_ms
                                ? (float)(ts_ms - im->tracker.first_ts_ms)
                                : 0.0f;
                            float velocity = elapsed_ms > 0.0f
                                ? im->tracker.total_distance /
                                  (elapsed_ms / 1000.0f) : 0.0f;
                            InputEvent *out = &events[event_count++];
                            memset(out, 0, sizeof(*out));
                            out->distance_ratio = distance_ratio;
                            out->velocity = velocity;
                            out->start_x = im->tracker.first_x;
                            out->start_y = im->tracker.first_y;
                            out->type = input_monitor_classify_release(
                                distance_ratio, im->swipe_thd,
                                im->tracker.first_x, im->tracker.first_y,
                                im->screen_width, im->screen_height,
                                im->gesture_thd_x, im->gesture_thd_y,
                                elapsed_ms, im->gesture_delay_time);

                            im->tracker.total_distance = 0.0f;
                            im->tracker.previous_x = -1;
                            im->tracker.previous_y = -1;
                            if (event_count >= max_events) goto done;
                        }
                    }
                    im->tracker.sample_count++;
                    im->tracker.last_ts_ms = ts_ms;
                    break;
            }
        }
    }

done:
    if (event_count > 0)
        log_debug("input_monitor: polled %d event(s)", event_count);
    return event_count;
}
