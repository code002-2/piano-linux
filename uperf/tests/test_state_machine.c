#include "../src/include/state_machine.h"
#include "../src/include/config.h"
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
#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("FAIL (%s: expected %d, got %d)\n", msg, (b), (a)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_GT(a, b, msg) do { \
    double _actual = (double)(a); \
    double _limit = (double)(b); \
    if (_actual <= _limit) { \
        printf("FAIL (%s: expected >%.3f, got %.3f)\n", msg, _limit, _actual); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_NEAR(a, b, eps, msg) do { \
    double _diff = fabs((double)(a) - (double)(b)); \
    if (_diff > (double)(eps)) { \
        printf("FAIL (%s: expected %.3f, got %.3f)\n", msg, \
               (double)(b), (double)(a)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

/* Create a minimal config for state machine tests */
static Config *make_test_config(void) {
    Config *cfg = calloc(1, sizeof(*cfg));
    cfg->cpu.nr_clusters = 3;
    cfg->switcher.hint_duration[SCENE_IDLE] = 0.0f;
    cfg->switcher.hint_duration[SCENE_TOUCH] = 4.0f;
    cfg->switcher.hint_duration[SCENE_TRIGGER] = 0.03f;
    cfg->switcher.hint_duration[SCENE_GESTURE] = 0.1f;
    cfg->switcher.hint_duration[SCENE_JUNK] = 0.06f;
    cfg->switcher.hint_duration[SCENE_SWITCH] = 0.4f;
    cfg->switcher.hint_duration[SCENE_BOOST] = 0.0f;
    cfg->initial_latency_time = 0.2f;
    cfg->initial_slow_limit_power = 3.0f;
    cfg->initial_fast_limit_power = 6.0f;
    cfg->initial_margin = 0.25f;
    cfg->presets[MODE_BALANCE].presets.global.margin = 0.35f;
    cfg->presets[MODE_BALANCE].presets.global.tuning_present =
        ACTION_TUNE_MARGIN;
    cfg->presets[MODE_BALANCE].presets.trigger.latency_time = 0.0f;
    cfg->presets[MODE_BALANCE].presets.trigger.tuning_present =
        ACTION_TUNE_LATENCY_TIME;
    cfg->presets[MODE_PERFORMANCE].presets.global.margin = 0.75f;
    cfg->presets[MODE_PERFORMANCE].presets.global.tuning_present =
        ACTION_TUNE_MARGIN;
    return cfg;
}

TEST(test_create_destroy) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);
    if (!sm) { printf("FAIL (null state machine)\n"); tests_failed++; return; }
    state_machine_free(sm);
    ASSERT_PASS("create and destroy");
}

TEST(test_initial_state) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_IDLE, "initial scene");
    ASSERT_EQ((int)state_machine_get_mode(sm), MODE_BALANCE, "initial mode");
    state_machine_free(sm);
    ASSERT_PASS("initial state is idle/balance");
}

TEST(test_touch_transition) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    SceneState s = state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    ASSERT_EQ((int)s, SCENE_TOUCH, "touch transition");
    state_machine_free(sm);
    ASSERT_PASS("idle -> touch on TOUCH_DOWN");
}

TEST(test_trigger_on_touch_up) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    SceneState s = state_machine_feed_event(sm, EVT_TOUCH_UP);
    ASSERT_EQ((int)s, SCENE_TRIGGER, "trigger transition");
    state_machine_free(sm);
    ASSERT_PASS("touch -> trigger on TOUCH_UP");
}

TEST(test_gesture_on_swipe) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    SceneState s = state_machine_feed_event(sm, EVT_GESTURE);
    ASSERT_EQ((int)s, SCENE_GESTURE, "gesture transition");
    state_machine_free(sm);
    ASSERT_PASS("touch -> gesture on GESTURE event");
}

TEST(test_swipe_event_enters_gesture_scene) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    SceneState s = state_machine_feed_event(sm, EVT_SWIPE);
    ASSERT_EQ((int)s, SCENE_GESTURE, "swipe transition");
    state_machine_free(sm);
    ASSERT_PASS("touch -> gesture on SWIPE event");
}

TEST(test_switch_on_window_event) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    SceneState s = state_machine_feed_event(sm, EVT_WINDOW_SWITCH);
    ASSERT_EQ((int)s, SCENE_SWITCH, "switch transition");
    state_machine_free(sm);
    ASSERT_PASS("touch -> switch on WINDOW_SWITCH event");
}

TEST(test_mode_change) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);  /* Go to touch */
    state_machine_set_mode(sm, MODE_PERFORMANCE);

    ASSERT_EQ((int)state_machine_get_mode(sm), MODE_PERFORMANCE, "mode changed");
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_IDLE, "reset to idle on mode change");
    state_machine_free(sm);
    ASSERT_PASS("mode change resets to idle");
}

TEST(test_boost_detection) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    bool needs = state_machine_needs_boost(sm, 80.0f, 60.0f);
    ASSERT_EQ(needs, true, "heavy load detected");
    state_machine_free(sm);
    ASSERT_PASS("boost needed when load > threshold");
}

TEST(test_boost_not_needed) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    bool needs = state_machine_needs_boost(sm, 10.0f, 60.0f);
    ASSERT_EQ(needs, false, "not heavy load");
    state_machine_free(sm);
    ASSERT_PASS("no boost when load < threshold");
}

TEST(test_timeout_from_trigger_to_touch) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    state_machine_feed_event(sm, EVT_TOUCH_UP);  /* trigger */
    state_machine_feed_event(sm, EVT_TIMEOUT);
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_TOUCH, "timeout to touch");
    state_machine_free(sm);
    ASSERT_PASS("trigger -> touch on timeout");
}

TEST(test_timeout_from_gesture_to_touch) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    state_machine_feed_event(sm, EVT_GESTURE);
    state_machine_feed_event(sm, EVT_TIMEOUT);
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_TOUCH, "timeout to touch");
    state_machine_free(sm);
    ASSERT_PASS("gesture -> touch on timeout");
}

TEST(test_timeout_from_switch_to_touch) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_WINDOW_SWITCH);
    state_machine_feed_event(sm, EVT_TIMEOUT);
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_TOUCH, "timeout to touch");
    state_machine_free(sm);
    ASSERT_PASS("switch -> touch on timeout");
}

TEST(test_boost_to_touch_on_heavy_end) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_HEAVY_LOAD_START);
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_BOOST, "entered boost");

    state_machine_feed_event(sm, EVT_HEAVY_LOAD_END);
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_TOUCH, "boost -> touch");
    state_machine_free(sm);
    ASSERT_PASS("boost exits to touch on heavy load end");
}

TEST(test_boost_to_idle_on_timeout) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_HEAVY_LOAD_START);
    state_machine_feed_event(sm, EVT_TIMEOUT);
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_IDLE, "boost -> idle");
    state_machine_free(sm);
    ASSERT_PASS("boost -> idle on timeout");
}

TEST(test_junk_from_touch) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    SceneState s = state_machine_feed_event(sm, EVT_JUNK);
    ASSERT_EQ((int)s, SCENE_JUNK, "junk transition");
    state_machine_free(sm);
    ASSERT_PASS("touch -> junk on junk event");
}

TEST(test_junk_to_touch_on_timeout) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    state_machine_feed_event(sm, EVT_JUNK);
    state_machine_feed_event(sm, EVT_TIMEOUT);
    ASSERT_EQ((int)state_machine_get_scene(sm), SCENE_TOUCH, "junk -> touch");
    state_machine_free(sm);
    ASSERT_PASS("junk -> touch on timeout");
}

TEST(test_multiple_mode_changes) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    state_machine_set_mode(sm, MODE_POWERSAVE);
    ASSERT_EQ((int)state_machine_get_mode(sm), MODE_POWERSAVE, "powersave mode");

    state_machine_set_mode(sm, MODE_PERFORMANCE);
    ASSERT_EQ((int)state_machine_get_mode(sm), MODE_PERFORMANCE, "performance mode");

    state_machine_set_mode(sm, MODE_BALANCE);
    ASSERT_EQ((int)state_machine_get_mode(sm), MODE_BALANCE, "balance mode");
    state_machine_free(sm);
    ASSERT_PASS("multiple mode changes work correctly");
}

TEST(test_get_hint_duration) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    float d = state_machine_get_hint_duration(sm, SCENE_TOUCH);
    ASSERT_GT(d, 0.0f, "touch hint duration > 0");
    state_machine_free(sm);
    ASSERT_PASS("hint duration retrievable");
}

TEST(test_get_actions) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    ActionParams params;
    state_machine_get_actions(sm, &params);
    /* Should return at least the initial defaults */
    ASSERT_GT(params.latency_time, 0.0f, "latency_time set");
    state_machine_free(sm);
    ASSERT_PASS("actions retrievable with defaults");
}

TEST(test_action_preset_inheritance_and_explicit_zero) {
    Config *cfg = make_test_config();
    StateMachine *sm = state_machine_new(cfg);
    free(cfg);

    ActionParams params;
    state_machine_get_actions(sm, &params);
    ASSERT_NEAR(params.margin, 0.35f, 0.001f, "mode global overrides initial");
    ASSERT_NEAR(params.latency_time, 0.2f, 0.001f, "initial inherited");

    state_machine_feed_event(sm, EVT_TOUCH_DOWN);
    state_machine_feed_event(sm, EVT_TOUCH_UP);
    state_machine_get_actions(sm, &params);
    ASSERT_NEAR(params.latency_time, 0.0f, 0.001f,
                "explicit zero scene override preserved");

    state_machine_set_mode(sm, MODE_PERFORMANCE);
    state_machine_get_actions(sm, &params);
    ASSERT_NEAR(params.margin, 0.75f, 0.001f,
                "mode-specific global selected");
    state_machine_free(sm);
    ASSERT_PASS("preset layers merge with explicit zero support");
}

int main(void) {
    printf("=== state_machine tests ===\n");
    log_init(UPERF_LOG_WARN, 0, NULL);

    RUN_TEST(test_create_destroy);
    RUN_TEST(test_initial_state);
    RUN_TEST(test_touch_transition);
    RUN_TEST(test_trigger_on_touch_up);
    RUN_TEST(test_gesture_on_swipe);
    RUN_TEST(test_swipe_event_enters_gesture_scene);
    RUN_TEST(test_switch_on_window_event);
    RUN_TEST(test_mode_change);
    RUN_TEST(test_boost_detection);
    RUN_TEST(test_boost_not_needed);
    RUN_TEST(test_timeout_from_trigger_to_touch);
    RUN_TEST(test_timeout_from_gesture_to_touch);
    RUN_TEST(test_timeout_from_switch_to_touch);
    RUN_TEST(test_boost_to_touch_on_heavy_end);
    RUN_TEST(test_boost_to_idle_on_timeout);
    RUN_TEST(test_junk_from_touch);
    RUN_TEST(test_junk_to_touch_on_timeout);
    RUN_TEST(test_multiple_mode_changes);
    RUN_TEST(test_get_hint_duration);
    RUN_TEST(test_get_actions);
    RUN_TEST(test_action_preset_inheritance_and_explicit_zero);

    printf("\nResults: %d/%d passed (%d failed)\n",
           tests_passed, tests_run, tests_failed);
    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
