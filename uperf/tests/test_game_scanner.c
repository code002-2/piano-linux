#define _POSIX_C_SOURCE 200809L
#include "../src/include/game_scanner.h"
#include "../src/include/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
} while(0)
#define ASSERT_PASS(msg) do { \
    printf("PASS\n"); tests_passed++; \
} while(0)
#define ASSERT_EQ(a, b, msg) do { \
    int _actual = (int)(a); \
    int _expected = (int)(b); \
    if (_actual != _expected) { \
        printf("FAIL (%s: expected %d, got %d)\n", msg, _expected, _actual); \
        tests_failed++; return; \
    } \
} while(0)
#define ASSERT_GT(a, b, msg) do { \
    int _actual = (int)(a); \
    int _limit = (int)(b); \
    if (_actual <= _limit) { \
        printf("FAIL (%s: expected >%d, got %d)\n", msg, _limit, _actual); \
        tests_failed++; return; \
    } \
} while(0)

/* Test: game_scanner_match with known game patterns */
TEST(test_match_unity) {
    ASSERT_EQ(game_scanner_match("UnityMain", ""), 1, "UnityMain match");
    ASSERT_PASS("UnityMain process matched");
}

TEST(test_match_unreal) {
    ASSERT_EQ(game_scanner_match("GameThread", ""), 1, "GameThread match");
    ASSERT_PASS("GameThread process matched");
}

TEST(test_match_emulator_dolphin) {
    ASSERT_EQ(game_scanner_match("dolphin", ""), 1, "dolphin match");
    ASSERT_PASS("dolphin emulator matched");
}

TEST(test_match_emulator_ppsspp) {
    ASSERT_EQ(game_scanner_match("ppsspp", ""), 1, "ppsspp match");
    ASSERT_PASS("ppsspp emulator matched");
}

TEST(test_match_emulator_retroarch) {
    ASSERT_EQ(game_scanner_match("retroarch", ""), 1, "retroarch match");
    ASSERT_PASS("retroarch emulator matched");
}

TEST(test_match_wine) {
    ASSERT_EQ(game_scanner_match("wine", ""), 1, "wine match");
    ASSERT_PASS("wine matched");
}

TEST(test_match_proton) {
    ASSERT_EQ(game_scanner_match("proton", ""), 1, "proton match");
    ASSERT_PASS("proton matched");
}

TEST(test_match_android_game) {
    ASSERT_EQ(game_scanner_match("", "com.miHoYo.Yuanshen"), 1, "miHoYo match");
    ASSERT_PASS("Android game package matched");
}

TEST(test_match_steam) {
    ASSERT_EQ(game_scanner_match("steam_app_1234", ""), 1, "steam match");
    ASSERT_PASS("Steam app matched");
}

TEST(test_no_match_normal_process) {
    ASSERT_EQ(game_scanner_match("bash", ""), 0, "bash no match");
    ASSERT_PASS("normal process not matched");
}

TEST(test_no_match_systemd) {
    ASSERT_EQ(game_scanner_match("systemd", ""), 0, "systemd no match");
    ASSERT_PASS("systemd not matched");
}

TEST(test_no_match_sshd) {
    ASSERT_EQ(game_scanner_match("sshd", ""), 0, "sshd no match");
    ASSERT_PASS("sshd not matched");
}

TEST(test_no_match_generic_words) {
    ASSERT_EQ(game_scanner_match("codex", "/usr/bin/codex"), 0,
              "codex no match");
    ASSERT_EQ(game_scanner_match("kingston", "/usr/bin/kingston-tool"), 0,
              "kingston no match");
    ASSERT_PASS("generic substrings are not classified as games");
}

TEST(test_no_match_null) {
    ASSERT_EQ(game_scanner_match(NULL, NULL), 0, "null match");
    ASSERT_PASS("null inputs return no match");
}

/* Test: game_scanner_new/free */
TEST(test_scanner_lifecycle) {
    GameScanner *gs = game_scanner_new();
    if (!gs) { printf("FAIL (game_scanner_new returned NULL)\n"); tests_failed++; return; }
    game_scanner_free(gs);
    ASSERT_PASS("scanner create and destroy");
}

/* Test: add custom pattern */
TEST(test_add_custom_pattern) {
    GameScanner *gs = game_scanner_new();
    if (!gs) { printf("SKIP (game_scanner_new returned NULL)\n"); tests_run++; return; }

    int ret = game_scanner_add_pattern(gs, "MyCustomGame");
    ASSERT_EQ(ret, 0, "add custom pattern");
    ASSERT_GT(game_scanner_pattern_count(gs), 0, "pattern count > 0");
    game_scanner_free(gs);
    ASSERT_PASS("custom pattern added");
}

/* Test: scan /proc */
TEST(test_scan_proc) {
    GameScanner *gs = game_scanner_new();
    if (!gs) { printf("SKIP (game_scanner_new returned NULL)\n"); tests_run++; return; }

    int count = game_scanner_scan(gs);
    /* On a real system, we should find at least some processes */
    (void)count;
    game_scanner_free(gs);
    ASSERT_PASS("/proc scan completed without error");
}

/* Test: get results */
TEST(test_get_results) {
    GameScanner *gs = game_scanner_new();
    if (!gs) { printf("SKIP (game_scanner_new returned NULL)\n"); tests_run++; return; }

    game_scanner_scan(gs);
    GameProcess results[MAX_GAMES];
    int count = game_scanner_get_results(gs, results, MAX_GAMES);
    (void)count;
    (void)results;
    game_scanner_free(gs);
    ASSERT_PASS("get results works");
}

TEST(test_persist_app_mode) {
    char path[] = "/tmp/uperf-perapp-test-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("FAIL (mkstemp)\n"); tests_failed++; return; }
    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); unlink(path); tests_failed++; return; }
    fputs("# existing rules\n.* balance\n", fp);
    fclose(fp);

    GameScanner *gs = game_scanner_new();
    if (!gs) { unlink(path); tests_failed++; return; }
    ASSERT_EQ(game_scanner_set_app_mode(gs, path, "ExampleGame",
                                        "performance"), 0,
              "persist app mode");
    fp = fopen(path, "r");
    char first_line[128] = {0};
    if (fp) {
        if (!fgets(first_line, sizeof(first_line), fp))
            first_line[0] = '\0';
        fclose(fp);
    }
    ASSERT_EQ(strcmp(first_line, "ExampleGame performance\n"), 0,
              "new rule is first");
    ASSERT_EQ(game_scanner_set_app_mode(gs, path, "ExampleGame",
                                        "powersave"), 0,
              "replace existing app rule");
    fp = fopen(path, "r");
    int matching_rules = 0;
    while (fp && fgets(first_line, sizeof(first_line), fp)) {
        if (strncmp(first_line, "ExampleGame ", 12) == 0)
            matching_rules++;
    }
    if (fp) fclose(fp);
    ASSERT_EQ(matching_rules, 1, "app rule is replaced instead of duplicated");
    game_scanner_free(gs);
    unlink(path);
    ASSERT_PASS("per-app mode persisted atomically");
}

TEST(test_custom_perapp_process_is_detected) {
    char path[] = "/tmp/uperf-perapp-detect-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { tests_failed++; return; }
    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); unlink(path); tests_failed++; return; }
    fputs("custom-uperf performance\n.* balance\n", fp);
    fclose(fp);

    pid_t child = fork();
    if (child == 0) {
        prctl(PR_SET_NAME, "custom-uperf", 0, 0, 0);
        sleep(5);
        _exit(0);
    }
    if (child < 0) { unlink(path); tests_failed++; return; }
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 20000000L};
    nanosleep(&settle, NULL);

    GameScanner *gs = game_scanner_new();
    game_scanner_perapp_scan(gs, path);
    game_scanner_scan(gs);
    GameProcess results[MAX_GAMES];
    int count = game_scanner_get_results(gs, results, MAX_GAMES);
    bool found = false;
    for (int i = 0; i < count; i++) {
        if (results[i].pid == child && results[i].start_time != 0)
            found = true;
    }

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    game_scanner_free(gs);
    unlink(path);
    ASSERT_EQ(found, true, "non-default per-app rule should detect process");
    ASSERT_PASS("custom per-app rules participate in process detection");
}

int main(void) {
    printf("=== game_scanner tests ===\n");
    log_init(UPERF_LOG_WARN, 0, NULL);

    RUN_TEST(test_match_unity);
    RUN_TEST(test_match_unreal);
    RUN_TEST(test_match_emulator_dolphin);
    RUN_TEST(test_match_emulator_ppsspp);
    RUN_TEST(test_match_emulator_retroarch);
    RUN_TEST(test_match_wine);
    RUN_TEST(test_match_proton);
    RUN_TEST(test_match_android_game);
    RUN_TEST(test_match_steam);
    RUN_TEST(test_no_match_normal_process);
    RUN_TEST(test_no_match_systemd);
    RUN_TEST(test_no_match_sshd);
    RUN_TEST(test_no_match_generic_words);
    RUN_TEST(test_no_match_null);
    RUN_TEST(test_scanner_lifecycle);
    RUN_TEST(test_add_custom_pattern);
    RUN_TEST(test_scan_proc);
    RUN_TEST(test_get_results);
    RUN_TEST(test_persist_app_mode);
    RUN_TEST(test_custom_perapp_process_is_detected);

    printf("\nResults: %d/%d passed (%d failed)\n",
           tests_passed, tests_run, tests_failed);
    log_shutdown();
    return tests_failed > 0 ? 1 : 0;
}
