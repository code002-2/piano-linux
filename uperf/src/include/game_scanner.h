#ifndef UPERF_GAME_SCANNER_H
#define UPERF_GAME_SCANNER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include "config.h"

/* Maximum length of a process command line */
#define CMDLINE_MAX 4096
#define MAX_PATTERNS  64

/* Detected process entry */
typedef struct {
    pid_t  pid;
    uint64_t start_time;       /* /proc/[pid]/stat field 22 */
    char   comm[16];           /* /proc/[pid]/comm */
    char   cmdline[CMDLINE_MAX]; /* /proc/[pid]/cmdline (null-separated) */
    bool   is_game;            /* matched as a game process */
    char   package[MAX_NAME_LEN]; /* derived name (e.g., "com.miHoYo.Yuanshen") */
} GameProcess;

/* Opaque game scanner handle */
typedef struct GameScanner GameScanner;

/* Create a new game scanner. */
GameScanner *game_scanner_new(void);

/* Free scanner resources. */
void game_scanner_free(GameScanner *gs);

/* Scan /proc for running processes matching game patterns.
 * Processes are stored internally; call game_scanner_get_results() to retrieve.
 * Returns number of processes found. */
int game_scanner_scan(GameScanner *gs);

/* Get the list of detected game processes.
 * out: caller-provided array of GameProcess (size >= MAX_GAMES).
 * Returns number of entries written. */
int game_scanner_get_results(GameScanner *gs, GameProcess *out, int max_entries);

/* Check if a process name/command line matches any known game pattern.
 * comm: process comm name.
 * cmdline: full command line.
 * Returns true if matched. */
bool game_scanner_match(const char *comm, const char *cmdline);

/* Register a custom game pattern (regex substring match).
 * pattern: substring to match against comm and cmdline.
 * Returns 0 on success. */
int game_scanner_add_pattern(GameScanner *gs, const char *pattern);

/* Get the number of registered patterns. */
int game_scanner_pattern_count(const GameScanner *gs);

/* Load per-app mode rules from a file.
 * Rules are applied during scan to assign modes to detected games.
 * Returns 0 on success, -1 on error. */
int game_scanner_perapp_scan(GameScanner *gs, const char *perapp_file);

/* Get the assigned power mode for a detected game by comm name.
 * Returns MODE_BALANCE if no rule matches. */
const char *game_scanner_get_app_mode(const GameScanner *gs, const char *comm);

/* Persist and immediately apply an exact per-app mode assignment. */
int game_scanner_set_app_mode(GameScanner *gs, const char *perapp_file,
                              const char *comm, const char *mode);

#endif /* UPERF_GAME_SCANNER_H */
