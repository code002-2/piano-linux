#include "power_model.h"
#include "log.h"

#include <math.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Power model: estimates the power draw of a cluster at a given
 * frequency, using piecewise-linear interpolation across the
 * performance-frequency curve regions.
 *
 * Regions (frequency in MHz):
 *   [0 .. freeFreq]       : Below minimum efficient frequency
 *   [freeFreq .. plain]   : Low-frequency efficiency ramp
 *   [plain .. sweet]      : Normal operating region
 *   [sweet .. typical]    : Steeper high-performance region
 * ---------------------------------------------------------------- */

float power_model_est_power(const PowerModelEntry *pm, float freq_mhz) {
    if (freq_mhz <= 0) return 0.0f;
    if (freq_mhz > pm->typical_freq_mhz)
        freq_mhz = pm->typical_freq_mhz;

    /* Normalize frequency to [0, 1] range relative to typical_freq */
    float norm = freq_mhz / pm->typical_freq_mhz;

    /* Power scales approximately as V×f ∝ f^(α) where α≈2-3 for CMOS.
     * We use a simplified quadratic model calibrated at typicalFreq. */
    float power = pm->typical_power_w * (norm * norm);

    /* Adjust for the freeFreq region (leakage dominates at low freq) */
    if (freq_mhz <= pm->free_freq_mhz) {
        /* At freeFreq, power is roughly 30% of typical */
        float free_power = pm->typical_power_w * 0.3f;
        power = free_power * (freq_mhz / pm->free_freq_mhz);
    }

    /* Low-frequency efficiency ramp. */
    if (freq_mhz > pm->free_freq_mhz &&
        freq_mhz <= pm->plain_freq_mhz) {
        float t = (freq_mhz - pm->free_freq_mhz) /
                  (pm->plain_freq_mhz - pm->free_freq_mhz);
        float free_p = pm->typical_power_w * 0.3f;
        float plain_p = pm->typical_power_w * 0.5f;
        power = free_p + t * (plain_p - free_p);
    }

    if (freq_mhz > pm->plain_freq_mhz &&
        freq_mhz <= pm->sweet_freq_mhz) {
        float t = (freq_mhz - pm->plain_freq_mhz) /
                  (pm->sweet_freq_mhz - pm->plain_freq_mhz);
        float plain_p = pm->typical_power_w * 0.5f;
        float sweet_p = pm->typical_power_w * 0.7f;
        power = plain_p + t * (sweet_p - plain_p);
    }

    if (freq_mhz > pm->sweet_freq_mhz) {
        float t = (freq_mhz - pm->sweet_freq_mhz) /
                  (pm->typical_freq_mhz - pm->sweet_freq_mhz);
        float sweet_p = pm->typical_power_w * 0.7f;
        power = sweet_p + t * (pm->typical_power_w - sweet_p);
    }

    return power;
}

float power_model_perf_at_freq(const PowerModelEntry *pm, float freq_mhz) {
    if (freq_mhz <= 0) return 0.0f;
    /* Performance is proportional to frequency, scaled by cluster efficiency */
    float norm = freq_mhz / pm->typical_freq_mhz;
    return pm->efficiency * norm;
}

float power_model_sweet_freq(const PowerModelEntry *pm) {
    return pm->sweet_freq_mhz;
}

float power_model_select_freq(const PowerModelEntry *pm,
                               float demand, float margin) {
    /* demand: required relative performance (0.0-1.0)
     * margin: additional headroom (0.0 = none, 1.0 = double)
     *
     * Target performance = demand * (1 + margin) * max_cluster_perf
     * Find the lowest frequency that meets this target.
     */
    float target_perf = demand * (1.0f + margin) * pm->efficiency;

    /* Binary search over frequency range */
    float lo = pm->free_freq_mhz;
    float hi = pm->typical_freq_mhz;
    float mid;

    for (int iter = 0; iter < 50; iter++) {
        mid = (lo + hi) / 2.0f;
        float perf = power_model_perf_at_freq(pm, mid);

        if (perf >= target_perf) {
            hi = mid;  /* Can go lower */
        } else {
            lo = mid;  /* Need higher frequency */
        }

        if (hi - lo < 1.0f)  /* Precision: 1 MHz */
            break;
    }

    return hi;
}

float power_model_compute_system_load(const int *load_pct,
                                      const float *freq_mhz,
                                      const PowerModelEntry *power_model,
                                      int nr_cpus, int nr_clusters) {
    /* system_load = Σ efficiency[i] × (load_pct[i]/100) × (freq_MHz[i]/1000)
     * This weights each CPU's contribution by its relative performance.
     *
     * This legacy aggregate API expects CPUs to be grouped contiguously in
     * the same order as power_model[].  Runtime policy control uses explicit
     * cpufreq masks via frequency_controller instead. */
    float total = 0.0f;
    int cpu_idx = 0;

    for (int c = 0; c < nr_clusters && power_model[c].nr_cores > 0; c++) {
        int cores_in_cluster = power_model[c].nr_cores;
        for (int j = 0; j < cores_in_cluster && cpu_idx < nr_cpus; j++, cpu_idx++) {
            float load_frac = load_pct[cpu_idx] / 100.0f;
            total += power_model[c].efficiency * load_frac *
                     (freq_mhz[cpu_idx] / 1000.0f);
        }
    }

    return total;
}

float power_model_total_power(const int *load_pct,
                              const float *freq_mhz,
                              const PowerModelEntry *power_model,
                              int nr_cpus, int nr_clusters) {
    /* Sum of per-cluster estimated power, weighted by load */
    float total = 0.0f;
    int cpu_idx = 0;

    for (int c = 0; c < nr_clusters && power_model[c].nr_cores > 0; c++) {
        int cores_in_cluster = power_model[c].nr_cores;
        for (int j = 0; j < cores_in_cluster && cpu_idx < nr_cpus; j++, cpu_idx++) {
            float load_frac = load_pct[cpu_idx] / 100.0f;
            float cluster_power = power_model_est_power(&power_model[c],
                                                         freq_mhz[cpu_idx]);
            total += cluster_power * load_frac;
        }
    }

    return total;
}
