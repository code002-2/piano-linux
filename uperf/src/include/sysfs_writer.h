#ifndef UPERF_SYSFS_WRITER_H
#define UPERF_SYSFS_WRITER_H

#include <stdint.h>
#include "config.h"

/* Maximum batch size: number of pending writes before forced flush */
#define SYSFS_BATCH_MAX  64

/* Write request queued for batching */
typedef struct {
    char path[MAX_PATH_LEN];
    char value[MAX_PATH_LEN];
    bool has_value;  /* false = delete/close (e.g. set governor to "" to reset) */
} WriteRequest;

/* SysfsWriter opaque handle */
typedef struct SysfsWriter SysfsWriter;

/* Create a new sysfs writer backed by the given config.
 * batch_window_ns: max time to accumulate writes before flushing (nanoseconds).
 *                  0 = flush immediately (no batching).
 * Returns NULL on failure. */
SysfsWriter *sysfs_writer_new(const Config *cfg, uint64_t batch_window_ns);

/* Destroy and free a sysfs writer. Flushes any pending writes first. */
void sysfs_writer_free(SysfsWriter *w);

/* Force an immediate flush of all pending writes. */
void sysfs_writer_flush(SysfsWriter *w);

/* Queue a single raw write. Returns 0 on success. */
int sysfs_writer_queue_raw(SysfsWriter *w, const char *path, const char *value);

/* Write a value immediately and report the kernel write result. This is used
 * for explicit user requests where success must not be acknowledged early. */
int sysfs_writer_write_raw(SysfsWriter *w, const char *path, const char *value);

/* Read current value of a sysfs path for dedup comparison.
 * Returns string in buf, or NULL on failure. Caller frees. */
char *sysfs_reader_read(const char *path);

#endif /* UPERF_SYSFS_WRITER_H */
