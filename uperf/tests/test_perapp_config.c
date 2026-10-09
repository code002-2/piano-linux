#include "perapp_config.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
#define ASSERT_OK(ret, msg) do { \
    if ((ret) != 0) { \
        printf("FAIL (%s: expected 0, got %d)\n", msg, (ret)); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_NULL(ptr, msg) do { \
    if ((ptr) != NULL) { \
        printf("FAIL (%s: expected NULL, got non-NULL)\n", msg); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)

/* Write a temporary perapp file for testing */
static int write_temp_perapp(const char *path, const char *content) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s", content);
    fclose(fp);
    return 0;
}

static void remove_temp(const char *path) {
    remove(path);
}

/* Test: load empty file */
TEST(test_load_empty_file) {
    const char *tmp = "/tmp/test_perapp_empty.conf";
    write_temp_perapp(tmp, "");
    PerAppConfig cfg;
    int ret = perapp_load(&cfg, tmp);
    ASSERT_OK(ret, "empty file should succeed");
    ASSERT_EQ(cfg.nr_rules, 0, "should have 0 rules");
    perapp_free(&cfg);
    remove_temp(tmp);
    ASSERT_PASS("empty file loaded");
}

/* Test: load file with comments only */
TEST(test_load_comments_only) {
    const char *tmp = "/tmp/test_perapp_comments.conf";
    write_temp_perapp(tmp, "# comment 1\n# comment 2\n\n");
    PerAppConfig cfg;
    int ret = perapp_load(&cfg, tmp);
    ASSERT_OK(ret, "comments-only file should succeed");
    ASSERT_EQ(cfg.nr_rules, 0, "should have 0 rules");
    perapp_free(&cfg);
    remove_temp(tmp);
    ASSERT_PASS("comments skipped");
}

/* Empty LF/CRLF lines must not be reported as malformed rules. */
TEST(test_load_crlf_and_blank_lines) {
    const char *tmp = "/tmp/test_perapp_crlf.conf";
    write_temp_perapp(tmp,
        "\r\n"
        " \t\r\n"
        "# comment\r\n"
        "com.test.crlf:performance\r\n");
    PerAppConfig cfg;
    int ret = perapp_load(&cfg, tmp);
    ASSERT_OK(ret, "CRLF file should succeed");
    ASSERT_EQ(cfg.nr_rules, 1, "blank CRLF lines should be skipped");
    ASSERT_STR_EQ(cfg.rules[0].pattern, "com.test.crlf", "CRLF pattern");
    ASSERT_EQ(cfg.rules[0].mode, MODE_PERFORMANCE, "CRLF mode");
    perapp_free(&cfg);
    remove_temp(tmp);
    ASSERT_PASS("CRLF and blank lines skipped");
}

/* Test: load colon-separated rules */
TEST(test_load_colon_separated) {
    const char *tmp = "/tmp/test_perapp_colon.conf";
    write_temp_perapp(tmp,
        "com.test.game:performance\n"
        "com.test.app:powersave\n");
    PerAppConfig cfg;
    int ret = perapp_load(&cfg, tmp);
    ASSERT_OK(ret, "colon-separated should load");
    ASSERT_EQ(cfg.nr_rules, 2, "should have 2 rules");
    ASSERT_STR_EQ(cfg.rules[0].pattern, "com.test.game", "first pattern");
    ASSERT_EQ(cfg.rules[0].mode, MODE_PERFORMANCE, "first mode");
    ASSERT_STR_EQ(cfg.rules[1].pattern, "com.test.app", "second pattern");
    ASSERT_EQ(cfg.rules[1].mode, MODE_POWERSAVE, "second mode");
    perapp_free(&cfg);
    remove_temp(tmp);
    ASSERT_PASS("colon-separated rules loaded");
}

/* Test: load space-separated rules */
TEST(test_load_space_separated) {
    const char *tmp = "/tmp/test_perapp_space.conf";
    write_temp_perapp(tmp,
        "minecraft balance\n"
        "retroarch performance\n");
    PerAppConfig cfg;
    int ret = perapp_load(&cfg, tmp);
    ASSERT_OK(ret, "space-separated should load");
    ASSERT_EQ(cfg.nr_rules, 2, "should have 2 rules");
    ASSERT_STR_EQ(cfg.rules[0].pattern, "minecraft", "first pattern");
    ASSERT_EQ(cfg.rules[0].mode, MODE_BALANCE, "first mode");
    ASSERT_STR_EQ(cfg.rules[1].pattern, "retroarch", "second pattern");
    ASSERT_EQ(cfg.rules[1].mode, MODE_PERFORMANCE, "second mode");
    perapp_free(&cfg);
    remove_temp(tmp);
    ASSERT_PASS("space-separated rules loaded");
}

/* Test: load with mixed formats and invalid lines */
TEST(test_load_mixed_formats) {
    const char *tmp = "/tmp/test_perapp_mixed.conf";
    write_temp_perapp(tmp,
        "# Header comment\n"
        "com.game1:performance\n"
        "invalid_line_no_sep\n"
        "com.game2  balance\n"
        "   \n"
        "com.game3:powersave\n");
    PerAppConfig cfg;
    int ret = perapp_load(&cfg, tmp);
    ASSERT_OK(ret, "mixed formats should load");
    ASSERT_EQ(cfg.nr_rules, 3, "should have 3 valid rules (skip invalid)");
    ASSERT_EQ(cfg.rules[0].mode, MODE_PERFORMANCE, "first");
    ASSERT_EQ(cfg.rules[1].mode, MODE_BALANCE, "second");
    ASSERT_EQ(cfg.rules[2].mode, MODE_POWERSAVE, "third");
    perapp_free(&cfg);
    remove_temp(tmp);
    ASSERT_PASS("mixed rules loaded");
}

/* Test: lookup by pattern match */
TEST(test_lookup_match) {
    const char *tmp = "/tmp/test_perapp_lookup.conf";
    write_temp_perapp(tmp, "com.miHoYo:performance\n");
    PerAppConfig cfg;
    perapp_load(&cfg, tmp);

    ASSERT_EQ(perapp_lookup(&cfg, "com.miHoYo.GenshinImpact"), MODE_PERFORMANCE,
              "substring match should work");
    ASSERT_EQ(perapp_lookup(&cfg, "COM.MIHOYO launcher"), MODE_PERFORMANCE,
              "matching is case insensitive");
    ASSERT_EQ(perapp_lookup(&cfg, "miHo"), MODE_BALANCE,
              "short process names must not reverse-match a rule");
    ASSERT_EQ(perapp_lookup(&cfg, "other.app"), MODE_BALANCE,
              "no match should return default");

    perapp_free(&cfg);
    remove_temp(tmp);
    ASSERT_PASS("pattern lookup works");
}

TEST(test_process_lookup_and_default_rule) {
    const char *tmp = "/tmp/test_perapp_process.conf";
    write_temp_perapp(tmp,
        "special-game performance\n"
        ".* balance\n");
    PerAppConfig cfg;
    perapp_load(&cfg, tmp);
    PowerMode mode = MODE_POWERSAVE;
    ASSERT_EQ(perapp_lookup_process(&cfg, "launcher",
                                    "/opt/special-game/bin", &mode), true,
              "cmdline rule should match before default");
    ASSERT_EQ(mode, MODE_PERFORMANCE, "cmdline selected performance");
    ASSERT_EQ(perapp_lookup_process(&cfg, "ordinary", NULL, &mode), true,
              "default wildcard should match");
    ASSERT_EQ(mode, MODE_BALANCE, "default wildcard mode");
    perapp_free(&cfg);
    remove_temp(tmp);
    ASSERT_PASS("process lookup preserves rule order and wildcard default");
}

/* Test: lookup with empty config */
TEST(test_lookup_empty) {
    PerAppConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(perapp_lookup(&cfg, "anything"), MODE_BALANCE,
              "empty config should return balance");
    ASSERT_PASS("empty lookup uses default");
}

/* Test: NULL safety */
TEST(test_null_safety) {
    ASSERT_EQ(perapp_lookup(NULL, "test"), MODE_BALANCE, "NULL cfg");
    ASSERT_EQ(perapp_lookup_pid(NULL, 1), MODE_BALANCE, "NULL cfg pid");
    ASSERT_PASS("NULL inputs handled");
}

int main(void) {
    /* Suppress log output during tests */
    log_init(UPERF_LOG_FATAL, 0, NULL);

    printf("=== Per-App Config Tests ===\n");

    RUN_TEST(test_load_empty_file);
    RUN_TEST(test_load_comments_only);
    RUN_TEST(test_load_crlf_and_blank_lines);
    RUN_TEST(test_load_colon_separated);
    RUN_TEST(test_load_space_separated);
    RUN_TEST(test_load_mixed_formats);
    RUN_TEST(test_lookup_match);
    RUN_TEST(test_process_lookup_and_default_rule);
    RUN_TEST(test_lookup_empty);
    RUN_TEST(test_null_safety);

    printf("\nResults: %d/%d passed, %d failed\n",
           tests_passed, tests_run, tests_failed);

    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
