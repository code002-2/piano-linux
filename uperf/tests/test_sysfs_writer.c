#define _POSIX_C_SOURCE 200809L
#include "../src/include/sysfs_writer.h"
#include "../src/include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

/* Test sysfs_reader_read on a known file */
TEST(test_read_known_file) {
    char *val = sysfs_reader_read("/proc/cpuinfo");
    if (val) {
        free(val);
        ASSERT_PASS("read /proc/cpuinfo successfully");
    } else {
        printf("SKIP (/proc/cpuinfo not readable)\n");
        tests_run++;
    }
}

/* Test sysfs_reader_read on nonexistent file */
TEST(test_read_nonexistent) {
    char *val = sysfs_reader_read("/nonexistent/path/that/does/not/exist");
    if (val == NULL) {
        ASSERT_PASS("returns NULL for nonexistent file");
    } else {
        free(val);
        printf("FAIL (should return NULL for nonexistent path)\n");
        tests_failed++;
    }
}

/* Test sysfs_reader_read on /proc/self/oom_score_adj (writable by user) */
TEST(test_read_proc_self) {
    char *val = sysfs_reader_read("/proc/self/oom_score_adj");
    if (val) {
        free(val);
        ASSERT_PASS("read /proc/self/oom_score_adj successfully");
    } else {
        printf("SKIP (/proc/self/oom_score_adj not readable)\n");
        tests_run++;
    }
}

/* Test sysfs_writer creation/destruction */
TEST(test_writer_lifecycle) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sysfs.nr_knobs = 0;

    SysfsWriter *w = sysfs_writer_new(&cfg, 0);  /* No batching */
    if (!w) {
        printf("FAIL (sysfs_writer_new returned NULL)\n");
        tests_failed++;
        return;
    }
    sysfs_writer_free(w);
    ASSERT_PASS("writer create and destroy");
}

/* Test sysfs_writer with batching enabled */
TEST(test_writer_with_batching) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sysfs.nr_knobs = 0;

    SysfsWriter *w = sysfs_writer_new(&cfg, 1000000);  /* 1ms batch window */
    if (!w) {
        printf("FAIL (sysfs_writer_new with batching returned NULL)\n");
        tests_failed++;
        return;
    }
    sysfs_writer_free(w);
    ASSERT_PASS("writer with batching created and destroyed");
}

/* Test queueing a raw write to a writable path */
TEST(test_queue_raw_write) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sysfs.nr_knobs = 0;

    SysfsWriter *w = sysfs_writer_new(&cfg, 0);
    if (!w) { printf("SKIP (writer creation failed)\n"); tests_run++; return; }

    int ret = sysfs_writer_queue_raw(w, "/proc/self/oom_score_adj", "0");
    (void)ret;
    sysfs_writer_free(w);
    ASSERT_PASS("queue raw write");
}

/* Test flush on empty writer */
TEST(test_flush_empty) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sysfs.nr_knobs = 0;

    SysfsWriter *w = sysfs_writer_new(&cfg, 0);
    if (!w) { printf("SKIP (writer creation failed)\n"); tests_run++; return; }

    sysfs_writer_flush(w);
    sysfs_writer_free(w);
    ASSERT_PASS("flush on empty writer succeeds");
}

/* Test queueing to a nonexistent path (should warn, not crash) */
TEST(test_queue_nonexistent_path) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.sysfs.nr_knobs = 0;

    SysfsWriter *w = sysfs_writer_new(&cfg, 0);
    if (!w) { printf("SKIP (writer creation failed)\n"); tests_run++; return; }

    /* Queue a write to a nonexistent path — should not crash */
    sysfs_writer_queue_raw(w, "/sys/nonexistent/knob", "1");
    sysfs_writer_flush(w);
    sysfs_writer_free(w);
    ASSERT_PASS("queue nonexistent path does not crash");
}

/* Test sysfs_reader_read on /sys/class/drm (if available) */
TEST(test_read_sysfs) {
    char *val = sysfs_reader_read("/sys/class/drm/card0/device/vendor");
    if (val) {
        free(val);
        ASSERT_PASS("read /sys/class/drm vendor successfully");
    } else {
        printf("SKIP (drm sysfs not available)\n");
        tests_run++;
    }
}

/* Test reader returns trimmed value */
TEST(test_reader_trims_newline) {
    char *val = sysfs_reader_read("/proc/self/comm");
    if (val) {
        /* Should not contain newline */
        if (strchr(val, '\n') == NULL) {
            ASSERT_PASS("reader trims trailing newline");
        } else {
            printf("FAIL (value contains newline)\n");
            tests_failed++;
        }
        free(val);
    } else {
        printf("SKIP (/proc/self/comm not readable)\n");
        tests_run++;
    }
}

TEST(test_immediate_and_full_batch_write) {
    char path[] = "/tmp/uperf-sysfs-writer-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("FAIL (mkstemp)\n"); tests_failed++; return; }
    close(fd);

    Config cfg = {0};
    SysfsWriter *w = sysfs_writer_new(&cfg, 1000000000ULL);
    if (!w) { unlink(path); tests_failed++; return; }
    if (sysfs_writer_write_raw(w, path, "A") != 0) {
        printf("FAIL (immediate write)\n");
        tests_failed++;
        sysfs_writer_free(w);
        unlink(path);
        return;
    }
    for (int i = 0; i < SYSFS_BATCH_MAX + 1; i++)
        sysfs_writer_queue_raw(w, path, i == SYSFS_BATCH_MAX ? "Z" : "B");
    sysfs_writer_flush(w);
    sysfs_writer_free(w);

    char *value = sysfs_reader_read(path);
    int ok = value && value[0] == 'Z';
    free(value);
    unlink(path);
    if (!ok) { printf("FAIL (full batch flush)\n"); tests_failed++; return; }
    ASSERT_PASS("immediate writes and full batches are flushed safely");
}

int main(void) {
    printf("=== sysfs_writer tests ===\n");
    log_init(UPERF_LOG_WARN, 0, NULL);

    RUN_TEST(test_read_known_file);
    RUN_TEST(test_read_nonexistent);
    RUN_TEST(test_read_proc_self);
    RUN_TEST(test_writer_lifecycle);
    RUN_TEST(test_writer_with_batching);
    RUN_TEST(test_queue_raw_write);
    RUN_TEST(test_flush_empty);
    RUN_TEST(test_queue_nonexistent_path);
    RUN_TEST(test_read_sysfs);
    RUN_TEST(test_reader_trims_newline);
    RUN_TEST(test_immediate_and_full_batch_write);

    printf("\nResults: %d/%d passed (%d failed)\n",
           tests_passed, tests_run, tests_failed);
    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
