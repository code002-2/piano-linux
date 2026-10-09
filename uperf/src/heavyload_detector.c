#define _POSIX_C_SOURCE 200809L
#include "heavyload_detector.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>
#include <ctype.h>

/* Internal heavy load detector state */
struct HeavyLoadDetector {
    float sample_time_ms;
    float heavy_load_threshold;
    float idle_load_threshold;
    float request_burst_slack_ms;

    /* Per-CPU stats */
    CpuStat *prev_stats;
    CpuStat *curr_stats;
    float   *cpu_loads;
    int nr_cpus;
    bool have_previous_sample;

    /* Load history for smoothing */
    float load_history[LOAD_HISTORY_SIZE];
    int   load_hist_idx;
    int   load_hist_count;

    /* Current state */
    bool heavy_load_active;
    float current_load;
    float smoothed_load;
    uint64_t last_sample_ms;
    uint64_t last_heavy_end_ms;
    uint64_t below_idle_since_ms;
};

bool heavyload_parse_cpu_stat_line(const char *line, int *cpu_id,
                                   CpuStat *stat) {
    if (!line || !cpu_id || !stat || strncmp(line, "cpu", 3) != 0 ||
        !isdigit((unsigned char)line[3]))
        return false;

    CpuStat parsed_stat = {0};
    int parsed_cpu = -1;
    int parsed = sscanf(line,
                        "cpu%d %" SCNu64 " %" SCNu64 " %" SCNu64
                        " %" SCNu64 " %" SCNu64 " %" SCNu64
                        " %" SCNu64 " %" SCNu64,
                        &parsed_cpu, &parsed_stat.user, &parsed_stat.nice,
                        &parsed_stat.system, &parsed_stat.idle,
                        &parsed_stat.iowait, &parsed_stat.irq,
                        &parsed_stat.softirq, &parsed_stat.steal);
    if (parsed < 8 || parsed_cpu < 0)
        return false;

    *cpu_id = parsed_cpu;
    *stat = parsed_stat;
    return true;
}

float heavyload_calculate_cpu_load(const CpuStat *previous,
                                   const CpuStat *current) {
    if (!previous || !current) return 0.0f;

    uint64_t previous_total = previous->user + previous->nice +
        previous->system + previous->idle + previous->iowait +
        previous->irq + previous->softirq + previous->steal;
    uint64_t current_total = current->user + current->nice +
        current->system + current->idle + current->iowait +
        current->irq + current->softirq + current->steal;
    if (current_total <= previous_total)
        return 0.0f;

    uint64_t delta_total = current_total - previous_total;
    uint64_t previous_idle = previous->idle + previous->iowait;
    uint64_t current_idle = current->idle + current->iowait;
    uint64_t delta_idle = current_idle >= previous_idle
        ? current_idle - previous_idle : 0;
    if (delta_idle > delta_total)
        delta_idle = delta_total;

    return (float)(delta_total - delta_idle) / (float)delta_total * 100.0f;
}

/* Read per-CPU stats from /proc/stat */
static int read_per_cpu_stats(CpuStat *stats, int nr_cpus) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    char line[256];
    int cpu_idx = 0;
    while (fgets(line, sizeof(line), fp) && cpu_idx < nr_cpus) {
        int cpu_id;
        CpuStat stat;
        if (heavyload_parse_cpu_stat_line(line, &cpu_id, &stat)) {
            (void)cpu_id;
            stats[cpu_idx] = stat;
            cpu_idx++;
        }
    }
    fclose(fp);
    return cpu_idx;
}

/* Get number of online CPUs */
static int get_nr_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 1;
}

static uint64_t monotonic_time_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static bool resize_cpu_arrays(HeavyLoadDetector *d, int nr_cpus) {
    CpuStat *previous = calloc((size_t)nr_cpus, sizeof(*previous));
    CpuStat *current = calloc((size_t)nr_cpus, sizeof(*current));
    float *loads = calloc((size_t)nr_cpus, sizeof(*loads));
    if (!previous || !current || !loads) {
        free(previous);
        free(current);
        free(loads);
        return false;
    }

    free(d->prev_stats);
    free(d->curr_stats);
    free(d->cpu_loads);
    d->prev_stats = previous;
    d->curr_stats = current;
    d->cpu_loads = loads;
    d->nr_cpus = nr_cpus;
    d->have_previous_sample = false;
    return true;
}

HeavyLoadDetector *heavyload_detector_new(float sample_time_ms,
                                          float heavy_load_threshold,
                                          float idle_load_threshold,
                                          float request_burst_slack_ms) {
    HeavyLoadDetector *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->sample_time_ms = sample_time_ms;
    d->heavy_load_threshold = heavy_load_threshold;
    d->idle_load_threshold = idle_load_threshold;
    d->request_burst_slack_ms = request_burst_slack_ms;
    if (!resize_cpu_arrays(d, get_nr_cpus())) {
        free(d);
        return NULL;
    }

    d->heavy_load_active = false;
    d->current_load = 0.0f;
    d->smoothed_load = 0.0f;
    d->load_hist_count = 0;
    d->load_hist_idx = 0;
    memset(d->load_history, 0, sizeof(d->load_history));

    log_info("HeavyLoadDetector created: nr_cpus=%d heavy_thresh=%.1f idle_thresh=%.1f",
             d->nr_cpus, heavy_load_threshold, idle_load_threshold);
    return d;
}

void heavyload_detector_free(HeavyLoadDetector *d) {
    if (!d) return;
    free(d->prev_stats);
    free(d->curr_stats);
    free(d->cpu_loads);
    log_debug("HeavyLoadDetector destroyed");
    free(d);
}

float heavyload_detector_sample(HeavyLoadDetector *d) {
    if (!d) return 0.0f;

    int online_cpus = get_nr_cpus();
    if (online_cpus != d->nr_cpus && !resize_cpu_arrays(d, online_cpus)) {
        log_error("heavyload_detector: failed to resize CPU arrays to %d",
                  online_cpus);
        return d->current_load;
    }

    /* Read current CPU stats */
    int nr = read_per_cpu_stats(d->curr_stats, d->nr_cpus);
    if (nr <= 0) {
        log_warn("heavyload_detector: failed to read /proc/stat");
        return d->current_load;
    }

    if (nr != d->nr_cpus) {
        log_warn("heavyload_detector: expected %d CPU rows, read %d",
                 d->nr_cpus, nr);
        return d->current_load;
    }

    if (!d->have_previous_sample) {
        memcpy(d->prev_stats, d->curr_stats,
               (size_t)nr * sizeof(*d->prev_stats));
        memset(d->cpu_loads, 0, (size_t)nr * sizeof(*d->cpu_loads));
        d->have_previous_sample = true;
        d->last_sample_ms = monotonic_time_ms();
        return 0.0f;
    }

    /* Calculate delta for each CPU */
    float total_load = 0.0f;
    for (int i = 0; i < nr; i++) {
        d->cpu_loads[i] = heavyload_calculate_cpu_load(
            &d->prev_stats[i], &d->curr_stats[i]);
        total_load += d->cpu_loads[i];

        /* Store for next iteration */
        d->prev_stats[i] = d->curr_stats[i];
    }

    /* Average across CPUs */
    d->current_load = total_load / (float)nr;

    /* Smooth with exponential moving average */
    if (d->load_hist_count == 0) {
        d->smoothed_load = d->current_load;
    } else {
        d->smoothed_load = d->smoothed_load * 0.7f + d->current_load * 0.3f;
    }

    /* Update load history ring buffer */
    d->load_history[d->load_hist_idx] = d->smoothed_load;
    d->load_hist_idx = (d->load_hist_idx + 1) % LOAD_HISTORY_SIZE;
    if (d->load_hist_count < LOAD_HISTORY_SIZE)
        d->load_hist_count++;

    /* Check heavy load state transitions */
    uint64_t now_ms = monotonic_time_ms();
    d->last_sample_ms = now_ms;

    if (!d->heavy_load_active && d->smoothed_load > d->heavy_load_threshold &&
        (d->last_heavy_end_ms == 0 ||
         now_ms - d->last_heavy_end_ms >= (uint64_t)d->request_burst_slack_ms)) {
        d->heavy_load_active = true;
        d->below_idle_since_ms = 0;
        log_info("HeavyLoad START: load=%.1f%% (threshold=%.1f%%)",
                 d->smoothed_load, d->heavy_load_threshold);
    } else if (d->heavy_load_active && d->smoothed_load < d->idle_load_threshold) {
        /* Must stay below idle threshold for a grace period */
        if (d->below_idle_since_ms == 0)
            d->below_idle_since_ms = now_ms;
        if (now_ms - d->below_idle_since_ms >= 1000) {
            d->heavy_load_active = false;
            d->last_heavy_end_ms = now_ms;
            d->below_idle_since_ms = 0;
            log_info("HeavyLoad END: load=%.1f%% (threshold=%.1f%%)",
                     d->smoothed_load, d->idle_load_threshold);
        }
    } else if (d->heavy_load_active) {
        d->below_idle_since_ms = 0;
    }

    return d->smoothed_load;
}

bool heavyload_detector_is_heavy(const HeavyLoadDetector *d) {
    if (!d) return false;
    return d->heavy_load_active;
}

float heavyload_detector_get_avg_load(const HeavyLoadDetector *d) {
    if (!d) return 0.0f;
    return d->smoothed_load;
}

float *heavyload_detector_get_cpu_loads(const HeavyLoadDetector *d, int *nr_cpus) {
    if (!d || !nr_cpus || d->nr_cpus <= 0) return NULL;

    float *snapshot = malloc((size_t)d->nr_cpus * sizeof(*snapshot));
    if (!snapshot) return NULL;
    *nr_cpus = d->nr_cpus;
    memcpy(snapshot, d->cpu_loads, (size_t)d->nr_cpus * sizeof(*snapshot));
    return snapshot;
}
