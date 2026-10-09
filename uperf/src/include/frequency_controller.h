#ifndef UPERF_FREQUENCY_CONTROLLER_H
#define UPERF_FREQUENCY_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Return the highest utilization among CPUs belonging to one cpufreq policy. */
float frequency_controller_cluster_demand(const float *cpu_loads,
                                          size_t nr_cpus,
                                          uint64_t cpu_mask);

/* Convert the current preset, utilization and thermal state into safe policy
 * limits.  All frequency arguments and results use Hz. */
void frequency_controller_compute_limits(const PowerModelEntry *model,
                                         const ActionParams *params,
                                         int cluster,
                                         float demand_percent,
                                         float thermal_reduction,
                                         int64_t hardware_min_hz,
                                         int64_t hardware_max_hz,
                                         int64_t *minimum_hz,
                                         int64_t *maximum_hz);

#endif /* UPERF_FREQUENCY_CONTROLLER_H */
