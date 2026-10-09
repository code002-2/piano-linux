#include "thermal.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("FAIL (%s: expected %d, got %d)\n", msg, (b), (a)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_STR_EQ(a, b, msg) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL (%s: expected '%s', got '%s')\n", msg, (b), (a)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_NEAR(a, b, delta, msg) do { \
    double _a = (a), _b = (b), _d = (delta); \
    if ((_a > _b ? _a - _b : _b - _a) > _d) { \
        printf("FAIL (%s: expected ~%.3f ±%.3f, got %.3f)\n", msg, _b, _d, _a); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_OK(ret, msg) do { \
    if ((ret) < 0) { \
        printf("FAIL (%s: expected >= 0, got %d)\n", msg, (ret)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_NOT_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { \
        printf("FAIL (%s: expected non-NULL)\n", msg); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

/* Test: create and destroy thermal manager */
TEST(test_create_destroy) {
    ThermalManager *tm = thermal_manager_new(NULL);
    ASSERT_NOT_NULL(tm, "should create with default policy");
    thermal_manager_free(tm);
    ASSERT_PASS("create/destroy");
}

/* Test: NULL safety */
TEST(test_null_safety) {
    ASSERT_EQ(thermal_manager_zone_count(NULL), 0, "NULL zone_count");
    ASSERT_EQ(thermal_manager_get_max_temp(NULL), 0, "NULL max_temp");
    ASSERT_EQ(thermal_manager_get_state(NULL), THERMAL_NORMAL, "NULL state");
    ASSERT_NEAR(thermal_manager_get_reduction_factor(NULL), 0.0f, 0.001f,
                "NULL reduction");
    ASSERT_EQ(thermal_manager_has_zone_type(NULL, THERMAL_ZONE_CPU), false, "NULL has_zone");
    ASSERT_EQ(thermal_manager_find_zone_by_type(NULL, THERMAL_ZONE_CPU), -1, "NULL find_zone");
    ASSERT_PASS("null safety");
}

/* Test: default policy values */
TEST(test_default_policy) {
    ThermalPolicy p = thermal_default_policy();
    ASSERT_EQ(p.warn_temp, 70000, "warn_temp");
    ASSERT_EQ(p.throttle_temp, 80000, "throttle_temp");
    ASSERT_EQ(p.critical_temp, 95000, "critical_temp");
    ASSERT_EQ(p.recovery_temp, 75000, "recovery_temp");
    ASSERT_PASS("default policy");
}

/* Test: conservative policy values */
TEST(test_conservative_policy) {
    ThermalPolicy p = thermal_conservative_policy();
    ASSERT_EQ(p.warn_temp, 60000, "conservative warn");
    ASSERT_EQ(p.throttle_temp, 70000, "conservative throttle");
    ASSERT_EQ(p.critical_temp, 85000, "conservative critical");
    ASSERT_EQ(p.recovery_temp, 65000, "conservative recovery");
    ASSERT_PASS("conservative policy");
}

/* Test: thermal state string conversion */
TEST(test_state_strings) {
    ASSERT_STR_EQ(thermal_state_to_string(THERMAL_NORMAL), "NORMAL", "normal");
    ASSERT_STR_EQ(thermal_state_to_string(THERMAL_WARNING), "WARNING", "warning");
    ASSERT_STR_EQ(thermal_state_to_string(THERMAL_THROTTLED), "THROTTLED", "throttled");
    ASSERT_STR_EQ(thermal_state_to_string(THERMAL_CRITICAL), "CRITICAL", "critical");
    ASSERT_PASS("state strings");
}

/* Test: a newly-created manager starts in the normal state. */
TEST(test_initial_reduction_factor) {
    ThermalManager *tm = thermal_manager_new(NULL);
    ASSERT_NOT_NULL(tm, "create manager");
    ASSERT_EQ(thermal_manager_get_state(tm), THERMAL_NORMAL, "initial state");
    ASSERT_NEAR(thermal_manager_get_reduction_factor(tm), 0.0f, 0.01f, "normal reduction");
    thermal_manager_free(tm);
    ASSERT_PASS("initial reduction factor");
}

/* Test: zone classification */
TEST(test_zone_classification) {
    /* classify_zone is static in thermal.c, so we test indirectly
     * through discover_zones which populates zone_type */
    /* Since we can't easily mock /sys/class/thermal, we verify
     * the manager handles the case of no thermal zones gracefully */
    ThermalManager *tm = thermal_manager_new(NULL);
    int nr = thermal_manager_discover_zones(tm);
    /* On systems without thermal framework, returns 0 */
    ASSERT_OK(nr, "discover_zones (may be 0 on non-ARM)");
    thermal_manager_free(tm);
    ASSERT_PASS("zone classification");
}

/* Test: policy with custom values */
TEST(test_custom_policy) {
    ThermalPolicy custom = {0};
    custom.warn_temp = 65000;
    custom.throttle_temp = 75000;
    custom.critical_temp = 90000;
    custom.recovery_temp = 70000;

    ThermalManager *tm = thermal_manager_new(&custom);
    ASSERT_NOT_NULL(tm, "create with custom policy");
    ASSERT_EQ(thermal_manager_get_state(tm), THERMAL_NORMAL, "custom initial state");
    ASSERT_NEAR(thermal_manager_get_reduction_factor(tm), 0.0f, 0.01f,
                "custom initial reduction");
    thermal_manager_free(tm);
    ASSERT_PASS("custom policy");
}

TEST(test_thermal_hysteresis) {
    ThermalPolicy policy = thermal_default_policy();
    ASSERT_EQ(thermal_policy_next_state(&policy, THERMAL_NORMAL, 71000),
              THERMAL_WARNING, "enter warning");
    ASSERT_EQ(thermal_policy_next_state(&policy, THERMAL_WARNING, 69000),
              THERMAL_WARNING, "warning deadband");
    ASSERT_EQ(thermal_policy_next_state(&policy, THERMAL_WARNING, 66000),
              THERMAL_NORMAL, "leave warning below deadband");
    ASSERT_EQ(thermal_policy_next_state(&policy, THERMAL_THROTTLED, 79000),
              THERMAL_THROTTLED, "throttle held until recovery");
    ASSERT_EQ(thermal_policy_next_state(&policy, THERMAL_THROTTLED, 74000),
              THERMAL_WARNING, "leave throttle below recovery");
    ASSERT_PASS("thermal transitions use hysteresis");
}

TEST(test_thermal_debounce) {
    ThermalState pending = THERMAL_NORMAL;
    int64_t since = -1;

    ASSERT_EQ(thermal_debounce_transition(THERMAL_NORMAL, THERMAL_WARNING,
                                          &pending, &since, 1000),
              THERMAL_NORMAL, "warning starts dwell");
    ASSERT_EQ(thermal_debounce_transition(THERMAL_NORMAL, THERMAL_WARNING,
                                          &pending, &since, 1999),
              THERMAL_NORMAL, "warning waits one second");
    ASSERT_EQ(thermal_debounce_transition(THERMAL_NORMAL, THERMAL_WARNING,
                                          &pending, &since, 2000),
              THERMAL_WARNING, "sustained warning accepted");

    ASSERT_EQ(thermal_debounce_transition(THERMAL_WARNING, THERMAL_NORMAL,
                                          &pending, &since, 3000),
              THERMAL_WARNING, "recovery starts dwell");
    ASSERT_EQ(thermal_debounce_transition(THERMAL_WARNING, THERMAL_NORMAL,
                                          &pending, &since, 4999),
              THERMAL_WARNING, "recovery waits two seconds");
    ASSERT_EQ(thermal_debounce_transition(THERMAL_WARNING, THERMAL_NORMAL,
                                          &pending, &since, 5000),
              THERMAL_NORMAL, "stable recovery accepted");

    ASSERT_EQ(thermal_debounce_transition(THERMAL_NORMAL, THERMAL_CRITICAL,
                                          &pending, &since, 5001),
              THERMAL_CRITICAL, "critical escalation is immediate");
    ASSERT_PASS("thermal transitions are time-debounced");
}

int main(void) {
    log_init(UPERF_LOG_FATAL, 0, NULL);

    printf("=== Thermal Manager Tests ===\n");

    RUN_TEST(test_create_destroy);
    RUN_TEST(test_null_safety);
    RUN_TEST(test_default_policy);
    RUN_TEST(test_conservative_policy);
    RUN_TEST(test_state_strings);
    RUN_TEST(test_initial_reduction_factor);
    RUN_TEST(test_zone_classification);
    RUN_TEST(test_custom_policy);
    RUN_TEST(test_thermal_hysteresis);
    RUN_TEST(test_thermal_debounce);

    printf("\nResults: %d/%d passed, %d failed\n",
           tests_passed, tests_run, tests_failed);

    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
