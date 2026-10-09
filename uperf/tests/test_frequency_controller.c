#include "../src/include/frequency_controller.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        failures++; \
    } \
} while (0)

static PowerModelEntry model(void) {
    PowerModelEntry value = {
        .efficiency = 300,
        .nr_cores = 3,
        .typical_power_w = 1.5f,
        .typical_freq_mhz = 3000.0f,
        .sweet_freq_mhz = 2200.0f,
        .plain_freq_mhz = 1600.0f,
        .free_freq_mhz = 600.0f,
    };
    return value;
}

static void test_cluster_demand(void) {
    float loads[] = {5.0f, 20.0f, 75.0f, 40.0f};
    CHECK(fabsf(frequency_controller_cluster_demand(loads, 4, 0xau) -
                40.0f) < 0.01f,
          "cluster demand uses only CPUs in the policy mask");
    CHECK(fabsf(frequency_controller_cluster_demand(loads, 4, 0x5u) -
                75.0f) < 0.01f,
          "cluster demand returns the busiest policy CPU");
}

static void test_limits(void) {
    PowerModelEntry pm = model();
    ActionParams params;
    memset(&params, 0, sizeof(params));
    params.margin = 0.2f;
    int64_t minimum, maximum;

    frequency_controller_compute_limits(&pm, &params, 0, 0.0f, 0.0f,
                                        300000000, 3000000000,
                                        &minimum, &maximum);
    CHECK(minimum >= 590000000 && minimum <= 610000000,
          "idle demand selects the model free frequency");
    CHECK(maximum == 3000000000, "normal mode preserves hardware maximum");

    params.limit_efficiency = true;
    frequency_controller_compute_limits(&pm, &params, 0, 100.0f, 0.0f,
                                        300000000, 3000000000,
                                        &minimum, &maximum);
    CHECK(maximum == 2200000000,
          "efficiency mode caps the policy at the sweet frequency");
    CHECK(minimum == maximum, "minimum never exceeds an efficiency cap");

    params.limit_efficiency = false;
    frequency_controller_compute_limits(&pm, &params, 0, 50.0f, 1.0f,
                                        300000000, 3000000000,
                                        &minimum, &maximum);
    CHECK(minimum == 300000000 && maximum == 300000000,
          "full thermal reduction clamps both limits to hardware minimum");

    memset(&params, 0, sizeof(params));
    params.burst = 0.5f;
    frequency_controller_compute_limits(&pm, &params, 0, 10.0f, 0.0f,
                                        300000000, 3000000000,
                                        &minimum, &maximum);
    CHECK(minimum >= 1790000000,
          "burst raises the requested floor above raw utilization");
}

int main(void) {
    test_cluster_demand();
    test_limits();
    if (failures == 0) puts("frequency_controller: all tests passed");
    return failures == 0 ? 0 : 1;
}
