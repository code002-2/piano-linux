#define _GNU_SOURCE
#include "sysfs_writer.h"
#include "log.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* Helper: monotonic nanoseconds */
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Internal sysfs writer state */
struct SysfsWriter {
    uint64_t batch_window_ns;
    uint64_t last_flush_ns;

    WriteRequest pending[SYSFS_BATCH_MAX];
    int nr_pending;

};

SysfsWriter *sysfs_writer_new(const Config *cfg, uint64_t batch_window_ns) {
    (void)cfg;
    SysfsWriter *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->batch_window_ns = batch_window_ns;
    w->last_flush_ns = now_ns();
    w->nr_pending = 0;

    log_info("SysfsWriter created: batch_window=%llu ns",
             (unsigned long long)batch_window_ns);
    return w;
}

void sysfs_writer_free(SysfsWriter *w) {
    if (!w) return;
    sysfs_writer_flush(w);
    free(w);
    log_debug("SysfsWriter destroyed");
}

/* Write a single value to a sysfs path */
static int write_sysfs_value(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        log_error("sysfs_writer: cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    ssize_t n = write(fd, value, strlen(value));
    if (n < 0) {
        log_error("sysfs_writer: write to %s failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

void sysfs_writer_flush(SysfsWriter *w) {
    if (!w) return;

    uint64_t now = now_ns();
    /* Process pending writes */
    for (int i = 0; i < w->nr_pending; i++) {
        if (w->pending[i].has_value) {
            write_sysfs_value(w->pending[i].path, w->pending[i].value);
        }
    }
    w->nr_pending = 0;
    w->last_flush_ns = now;
}

int sysfs_writer_queue_raw(SysfsWriter *w, const char *path, const char *value) {
    if (!w) return -1;
    if (!path || !value) return -1;
    if (w->nr_pending >= SYSFS_BATCH_MAX) {
        sysfs_writer_flush(w);
    }

    WriteRequest *req = &w->pending[w->nr_pending++];
    strncpy(req->path, path, MAX_PATH_LEN - 1);
    req->path[MAX_PATH_LEN - 1] = '\0';
    strncpy(req->value, value, MAX_PATH_LEN - 1);
    req->value[MAX_PATH_LEN - 1] = '\0';
    req->has_value = true;

    return 0;
}

int sysfs_writer_write_raw(SysfsWriter *w, const char *path, const char *value) {
    if (!w || !path || !value) return -1;
    return write_sysfs_value(path, value);
}

char *sysfs_reader_read(const char *path) {
    if (!path) return NULL;

    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);

    if (n == 0) return NULL;
    buf[n] = '\0';

    /* Strip trailing whitespace */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';

    char *result = strdup(buf);
    return result;
}
