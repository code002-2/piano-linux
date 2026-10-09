#include "frequency_controller.h"

#include "power_model.h"

#include <math.h>

static float clamp_float(float value, float minimum, float maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int64_t clamp_frequency(int64_t value, int64_t minimum,
                               int64_t maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

float frequency_controller_cluster_demand(const float *cpu_loads,
                                          size_t nr_cpus,
                                          uint64_t cpu_mask) {
    if (!cpu_loads || nr_cpus == 0 || cpu_mask == 0) return 0.0f;

    float maximum = 0.0f;
    size_t limit = nr_cpus < 64 ? nr_cpus : 64;
    for (size_t cpu = 0; cpu < limit; cpu++) {
        if ((cpu_mask & (UINT64_C(1) << cpu)) != 0 &&
            cpu_loads[cpu] > maximum)
            maximum = cpu_loads[cpu];
    }
    return clamp_float(maximum, 0.0f, 100.0f);
}

void frequency_controller_compute_limits(const PowerModelEntry *model,
                                         const ActionParams *params,
                                         int cluster,
                                         float demand_percent,
                                         float thermal_reduction,
                                         int64_t hardware_min_hz,
                                         int64_t hardware_max_hz,
                                         int64_t *minimum_hz,
                                         int64_t *maximum_hz) {
    if (!minimum_hz || !maximum_hz) return;
    *minimum_hz = hardware_min_hz;
    *maximum_hz = hardware_max_hz;
    if (!model || !params || hardware_min_hz <= 0 ||
        hardware_max_hz < hardware_min_hz)
        return;

    float demand = clamp_float(demand_percent / 100.0f, 0.0f, 1.0f);
    demand = clamp_float(demand + fmaxf(params->burst, 0.0f), 0.0f, 1.0f);
    float margin = fmaxf(params->margin, 0.0f);
    int64_t selected_hz = (int64_t)llroundf(
        power_model_select_freq(model, demand, margin) * 1000000.0f);

    int64_t min_hz = clamp_frequency(selected_hz, hardware_min_hz,
                                     hardware_max_hz);
    int64_t max_hz = hardware_max_hz;

    if (params->limit_efficiency && model->sweet_freq_mhz > 0.0f) {
        int64_t sweet_hz = (int64_t)llroundf(model->sweet_freq_mhz *
                                             1000000.0f);
        max_hz = clamp_frequency(sweet_hz, hardware_min_hz, max_hz);
    }

    if (params->has_cpu_freq_min && cluster >= 0 &&
        cluster < MAX_CLUSTERS && params->cpu_freq_min[cluster] > 0)
        min_hz = clamp_frequency(params->cpu_freq_min[cluster],
                                 hardware_min_hz, hardware_max_hz);
    if (params->has_cpu_freq_max && cluster >= 0 &&
        cluster < MAX_CLUSTERS && params->cpu_freq_max[cluster] > 0)
        max_hz = clamp_frequency(params->cpu_freq_max[cluster],
                                 hardware_min_hz, hardware_max_hz);

    float thermal = clamp_float(thermal_reduction, 0.0f, 1.0f);
    if (thermal > 0.0f) {
        double range = (double)(max_hz - hardware_min_hz);
        max_hz = hardware_min_hz + (int64_t)llround(range * (1.0 - thermal));
    }

    if (min_hz > max_hz) min_hz = max_hz;
    *minimum_hz = min_hz;
    *maximum_hz = max_hz;
}
