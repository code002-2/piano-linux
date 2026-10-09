#define _POSIX_C_SOURCE 200809L
#include "heavyload_detector.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void test_proc_stat_parser(void) {
    CpuStat stat;
    int cpu_id = -1;

    assert(heavyload_parse_cpu_stat_line(
        "cpu12 100 2 30 400 5 6 7 8 9 10\n", &cpu_id, &stat));
    assert(cpu_id == 12);
    assert(stat.user == 100);
    assert(stat.idle == 400);
    assert(stat.steal == 8);

    assert(!heavyload_parse_cpu_stat_line(
        "cpu 100 2 30 400 5 6 7 8\n", &cpu_id, &stat));
    assert(!heavyload_parse_cpu_stat_line("intr 123\n", &cpu_id, &stat));
    assert(!heavyload_parse_cpu_stat_line("cpu0 1 2 3\n", &cpu_id, &stat));
}

static void test_delta_math(void) {
    CpuStat previous = {
        .user = 100, .system = 50, .idle = 800, .iowait = 50,
    };
    CpuStat current = {
        .user = 140, .system = 60, .idle = 840, .iowait = 50,
    };

    /* 50 busy jiffies out of 90 total. */
    float load = heavyload_calculate_cpu_load(&previous, &current);
    assert(fabsf(load - 55.555f) < 0.01f);

    assert(heavyload_calculate_cpu_load(&current, &current) == 0.0f);
    assert(heavyload_calculate_cpu_load(&current, &previous) == 0.0f);
}

static void test_live_sample(void) {
    HeavyLoadDetector *detector = heavyload_detector_new(50.0f, 90.0f,
                                                          10.0f, 250.0f);
    assert(detector != NULL);
    assert(heavyload_detector_sample(detector) == 0.0f);
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&delay, NULL);
    float load = heavyload_detector_sample(detector);
    assert(load >= 0.0f && load <= 100.0f);

    int nr_cpus = 0;
    float *loads = heavyload_detector_get_cpu_loads(detector, &nr_cpus);
    assert(loads != NULL);
    assert(nr_cpus > 0);
    for (int i = 0; i < nr_cpus; i++)
        assert(loads[i] >= 0.0f && loads[i] <= 100.0f);
    free(loads);
    heavyload_detector_free(detector);
}

int main(void) {
    test_proc_stat_parser();
    test_delta_math();
    test_live_sample();
    puts("heavyload detector tests passed");
    return 0;
}
