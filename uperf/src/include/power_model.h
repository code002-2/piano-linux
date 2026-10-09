#ifndef UPERF_POWER_MODEL_H
#define UPERF_POWER_MODEL_H

#include <stdint.h>

/* Power model entry for one CPU cluster.
 * Values are calibrated empirically per SoC.
 * Efficiency is normalized to Cortex-A53@1GHz = 100. */
typedef struct {
    int    efficiency;       /* Relative single-core performance score */
    int    nr_cores;         /* Number of cores in this cluster */
    uint64_t cpu_mask;       /* Explicit CPUs belonging to this model */
    char   cpumask_name[32]; /* Optional modules.sched.cpumask reference */
    float  typical_power_w;  /* Single-core power at typical_freq (Watts) */
    float  typical_freq_mhz; /* Normal operating frequency (MHz) */
    float  sweet_freq_mhz;   /* Most energy-efficient frequency (MHz) */
    float  plain_freq_mhz;   /* Linear region boundary (MHz) */
    float  free_freq_mhz;    /* Minimum efficient frequency (MHz) */
} PowerModelEntry;

/* Compute estimated power draw for one cluster at a given frequency.
 * freq_mhz: target frequency in MHz.
 * Returns estimated power in Watts. */
float power_model_est_power(const PowerModelEntry *pm, float freq_mhz);

/* Compute performance at a given frequency.
 * Returns relative performance score (normalized to efficiency at typical_freq). */
float power_model_perf_at_freq(const PowerModelEntry *pm, float freq_mhz);

/* Find the "sweet spot" frequency — the one with best performance-per-watt ratio.
 * Searches within the cluster's frequency range.
 * Returns sweet_freq_mhz from the power model entry (pre-calibrated). */
float power_model_sweet_freq(const PowerModelEntry *pm);

/* Select optimal frequency for a given performance demand.
 * demand: required relative performance (0.0-1.0, where 1.0 = max cluster perf).
 * margin: additional headroom factor (0.0 = none, 1.0 = double).
 * Returns optimal frequency in MHz. */
float power_model_select_freq(const PowerModelEntry *pm,
                              float demand, float margin);

/* Compute system-wide load from per-CPU utilization samples.
 * load_pct[]: per-CPU load percentage (0-100).
 * freq_mhz[]: per-CPU current frequency in MHz.
 * power_model: array of cluster entries.
 * nr_cpus: number of CPUs sampled.
 * nr_clusters: number of entries in power_model.
 * Returns weighted load score. */
float power_model_compute_system_load(const int *load_pct,
                                      const float *freq_mhz,
                                      const PowerModelEntry *power_model,
                                      int nr_cpus, int nr_clusters);

/* Compute the total power budget for all clusters given current loads.
 * Returns total estimated system power in Watts. */
float power_model_total_power(const int *load_pct,
                              const float *freq_mhz,
                              const PowerModelEntry *power_model,
                              int nr_cpus, int nr_clusters);

#endif /* UPERF_POWER_MODEL_H */
