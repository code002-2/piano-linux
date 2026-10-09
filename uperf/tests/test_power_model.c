#include "../src/include/power_model.h"
#include "../src/include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_NEAR(a, b, eps, msg) do { \
    double diff = fabs((double)(a) - (double)(b)); \
    if (diff > (eps)) { \
        printf("FAIL (%s: expected ~%.2f, got %.2f, diff %.4f)\n", \
               msg, (double)(b), (double)(a), diff); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_GT(a, b, msg) do { \
    if ((a) <= (b)) { \
        printf("FAIL (%s: expected %.2f > %.2f)\n", msg, (double)(a), (double)(b)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_LT(a, b, msg) do { \
    if ((a) >= (b)) { \
        printf("FAIL (%s: expected %.2f < %.2f)\n", msg, (double)(a), (double)(b)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

/* Create a test power model entry */
static PowerModelEntry make_pm(int eff, int nr, float typ_p, float typ_f,
                                float sweet_f, float plain_f, float free_f) {
    PowerModelEntry pm = {0};
    pm.efficiency = eff;
    pm.nr_cores = nr;
    pm.typical_power_w = typ_p;
    pm.typical_freq_mhz = typ_f;
    pm.sweet_freq_mhz = sweet_f;
    pm.plain_freq_mhz = plain_f;
    pm.free_freq_mhz = free_f;
    return pm;
}

/* === Power estimation tests === */

TEST(test_zero_freq_zero_power) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float p = power_model_est_power(&pm, 0.0f);
    ASSERT_NEAR(p, 0.0f, 0.01, "zero freq → zero power");
    ASSERT_PASS("zero frequency returns zero power");
}

TEST(test_power_increases_with_freq) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float p_low = power_model_est_power(&pm, 600.0f);
    float p_high = power_model_est_power(&pm, 2000.0f);
    ASSERT_GT(p_high, p_low, "higher freq → more power");
    ASSERT_PASS("power increases with frequency");
}

TEST(test_power_at_typical_freq) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float p = power_model_est_power(&pm, pm.typical_freq_mhz);
    /* At typical freq, power should be close to typical_power */
    ASSERT_NEAR(p, pm.typical_power_w, 0.5f, "power at typical freq");
    ASSERT_PASS("power at typical frequency is reasonable");
}

TEST(test_power_monotonic_above_sweet) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    /* Power should be strictly increasing above sweet spot */
    float p1 = power_model_est_power(&pm, pm.sweet_freq_mhz + 100);
    float p2 = power_model_est_power(&pm, pm.sweet_freq_mhz + 500);
    float p3 = power_model_est_power(&pm, pm.sweet_freq_mhz + 1000);
    ASSERT_GT(p2, p1, "p2 > p1");
    ASSERT_GT(p3, p2, "p3 > p2");
    ASSERT_PASS("power is monotonically increasing above sweet spot");
}

TEST(test_power_below_free) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float p = power_model_est_power(&pm, pm.free_freq_mhz * 0.5f);
    ASSERT_GT(p, 0.0f, "power > 0 even below free freq");
    ASSERT_PASS("power below free frequency is small but positive");
}

/* === Performance tests === */

TEST(test_perf_at_zero_freq) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float perf = power_model_perf_at_freq(&pm, 0.0f);
    ASSERT_NEAR(perf, 0.0f, 0.01, "zero perf at zero freq");
    ASSERT_PASS("zero frequency yields zero performance");
}

TEST(test_perf_linear_with_freq) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float perf_half = power_model_perf_at_freq(&pm, 1100.0f);
    float perf_full = power_model_perf_at_freq(&pm, 2200.0f);
    ASSERT_GT(perf_full, perf_half, "full freq > half freq perf");
    ASSERT_NEAR(perf_full / perf_half, 2.0f, 0.3f, "perf ratio ≈ 2:1");
    ASSERT_PASS("performance scales roughly linearly with frequency");
}

TEST(test_perf_at_typical_is_efficiency) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float perf = power_model_perf_at_freq(&pm, pm.typical_freq_mhz);
    ASSERT_NEAR(perf, (float)pm.efficiency, 10.0f, "perf at typical ≈ efficiency");
    ASSERT_PASS("performance at typical frequency equals efficiency score");
}

TEST(test_different_clusters_different_perf) {
    PowerModelEntry big = make_pm(700, 1, 3.0f, 3000, 2400, 1800, 800);
    PowerModelEntry little = make_pm(180, 5, 0.5f, 1200, 800, 500, 300);

    float perf_big = power_model_perf_at_freq(&big, 2000.0f);
    float perf_lit = power_model_perf_at_freq(&little, 1000.0f);
    ASSERT_GT(perf_big, perf_lit, "big cluster > little cluster");
    ASSERT_PASS("different clusters have different performance profiles");
}

/* === Sweet spot tests === */

TEST(test_sweet_spot_returns_correct_freq) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float sweet = power_model_sweet_freq(&pm);
    ASSERT_NEAR(sweet, 1600.0f, 1.0f, "sweet freq");
    ASSERT_PASS("sweet spot returns calibrated frequency");
}

TEST(test_sweet_freq_between_free_and_typical) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float sweet = power_model_sweet_freq(&pm);
    ASSERT_GT(sweet, pm.free_freq_mhz, "sweet > free");
    ASSERT_LT(sweet, pm.typical_freq_mhz, "sweet < typical");
    ASSERT_PASS("sweet spot is between free and typical frequency");
}

/* === Frequency selection tests === */

TEST(test_select_freq_meets_demand) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float freq = power_model_select_freq(&pm, 0.5f, 0.0f);
    ASSERT_GT(freq, 0.0f, "non-zero freq for positive demand");
    ASSERT_LT(freq, pm.typical_freq_mhz * 2.0f, "freq within bounds");
    ASSERT_PASS("select_freq returns reasonable frequency for 50% demand");
}

TEST(test_select_freq_with_margin) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float freq_low = power_model_select_freq(&pm, 0.3f, 0.0f);
    float freq_high = power_model_select_freq(&pm, 0.3f, 0.5f);
    ASSERT_GT(freq_high, freq_low, "margin increases frequency");
    ASSERT_PASS("margin parameter increases selected frequency");
}

TEST(test_select_freq_high_demand) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float freq = power_model_select_freq(&pm, 0.9f, 0.0f);
    ASSERT_GT(freq, 1000.0f, "high demand → high frequency");
    ASSERT_PASS("high demand selects high frequency");
}

TEST(test_select_freq_zero_demand) {
    PowerModelEntry pm = make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500);
    float freq = power_model_select_freq(&pm, 0.0f, 0.0f);
    ASSERT_NEAR(freq, pm.free_freq_mhz, 100.0f, "zero demand → near free freq");
    ASSERT_PASS("zero demand selects minimum frequency");
}

/* === Load computation tests === */

TEST(test_system_load_computation) {
    PowerModelEntry pm[1] = { make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500) };
    int loads[2] = {50, 50};
    float freqs[2] = {2200, 2200};
    float load = power_model_compute_system_load(loads, freqs, pm, 2, 1);
    ASSERT_GT(load, 0.0f, "positive load");
    ASSERT_PASS("system load computed from per-CPU samples");
}

TEST(test_system_load_zero) {
    PowerModelEntry pm[1] = { make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500) };
    int loads[2] = {0, 0};
    float freqs[2] = {1000, 1000};
    float load = power_model_compute_system_load(loads, freqs, pm, 2, 1);
    ASSERT_NEAR(load, 0.0f, 1.0f, "zero load at zero utilization");
    ASSERT_PASS("system load is zero when all CPUs idle");
}

TEST(test_system_load_max) {
    PowerModelEntry pm[1] = { make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500) };
    int loads[2] = {100, 100};
    float freqs[2] = {2200, 2200};
    float load = power_model_compute_system_load(loads, freqs, pm, 2, 1);
    /* Expected: 280 * 1.0 * 2.2 = 616 */
    ASSERT_GT(load, 500.0f, "high load at max utilization");
    ASSERT_PASS("system load is high at full utilization");
}

TEST(test_system_load_different_clusters) {
    PowerModelEntry pm[2] = {
        make_pm(350, 1, 1.2f, 2400, 1800, 1400, 600),  /* Prime */
        make_pm(180, 5, 0.8f, 1600, 1000, 700, 300)    /* Efficiency */
    };
    int loads[2] = {100, 100};
    float freqs[2] = {2400, 1600};
    float load = power_model_compute_system_load(loads, freqs, pm, 2, 2);
    /* Prime contributes: 350 * 1.0 * 2.4 = 840 */
    /* Eff contributes: 180 * 1.0 * 1.6 = 288 */
    /* Total: ~1128 */
    ASSERT_GT(load, 1000.0f, "multi-cluster load");
    ASSERT_PASS("load computation works with multiple clusters");
}

/* === Total power tests === */

TEST(total_power_at_zero_load) {
    PowerModelEntry pm[1] = { make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500) };
    int loads[2] = {0, 0};
    float freqs[2] = {1000, 1000};
    float total = power_model_total_power(loads, freqs, pm, 2, 1);
    ASSERT_LT(total, 1.0f, "low power at zero load");
    ASSERT_PASS("total power near-zero at zero load");
}

TEST(total_power_increases_with_load) {
    PowerModelEntry pm[1] = { make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500) };
    int loads_low[2] = {10, 10};
    int loads_high[2] = {90, 90};
    float freqs[2] = {2000, 2000};
    float p_low = power_model_total_power(loads_low, freqs, pm, 2, 1);
    float p_high = power_model_total_power(loads_high, freqs, pm, 2, 1);
    ASSERT_GT(p_high, p_low, "high load → more power");
    ASSERT_PASS("total power increases with load");
}

TEST(total_power_with_different_frequencies) {
    PowerModelEntry pm[1] = { make_pm(280, 2, 1.5f, 2200, 1600, 1200, 500) };
    int loads[2] = {50, 50};
    float freqs_low[2] = {600, 600};
    float freqs_high[2] = {2000, 2000};
    float p_low = power_model_total_power(loads, freqs_low, pm, 2, 1);
    float p_high = power_model_total_power(loads, freqs_high, pm, 2, 1);
    ASSERT_GT(p_high, p_low, "high freq → more power");
    ASSERT_PASS("total power increases with frequency");
}

int main(void) {
    printf("=== power_model tests ===\n");
    log_init(UPERF_LOG_WARN, 0, NULL);

    RUN_TEST(test_zero_freq_zero_power);
    RUN_TEST(test_power_increases_with_freq);
    RUN_TEST(test_power_at_typical_freq);
    RUN_TEST(test_power_monotonic_above_sweet);
    RUN_TEST(test_power_below_free);
    RUN_TEST(test_perf_at_zero_freq);
    RUN_TEST(test_perf_linear_with_freq);
    RUN_TEST(test_perf_at_typical_is_efficiency);
    RUN_TEST(test_different_clusters_different_perf);
    RUN_TEST(test_sweet_spot_returns_correct_freq);
    RUN_TEST(test_sweet_freq_between_free_and_typical);
    RUN_TEST(test_select_freq_meets_demand);
    RUN_TEST(test_select_freq_with_margin);
    RUN_TEST(test_select_freq_high_demand);
    RUN_TEST(test_select_freq_zero_demand);
    RUN_TEST(test_system_load_computation);
    RUN_TEST(test_system_load_zero);
    RUN_TEST(test_system_load_max);
    RUN_TEST(test_system_load_different_clusters);
    RUN_TEST(total_power_at_zero_load);
    RUN_TEST(total_power_increases_with_load);
    RUN_TEST(total_power_with_different_frequencies);

    printf("\nResults: %d/%d passed (%d failed)\n",
           tests_passed, tests_run, tests_failed);
    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
