#ifndef UPERF_PERAPP_CONFIG_H
#define UPERF_PERAPP_CONFIG_H

#include "config.h"
#include <sys/types.h>

/* Maximum number of per-app rules */
#define PERAPP_MAX_RULES 64

/* A single per-app rule: pattern → power mode mapping */
typedef struct {
    char pattern[MAX_NAME_LEN];  /* Process comm or package name substring */
    PowerMode mode;
} PerAppRule;

/* Per-app configuration — loaded from a text file */
typedef struct {
    PerAppRule rules[PERAPP_MAX_RULES];
    int nr_rules;
    char file_path[MAX_PATH_LEN];  /* Source file path */
} PerAppConfig;

/* Load per-app rules from a text file.
 * Format: one rule per line, "pattern:mode" where mode is
 * balance|powersave|performance|fast. Lines starting with # are comments.
 * Returns 0 on success, -1 on error. */
int perapp_load(PerAppConfig *cfg, const char *path);

/* Look up the power mode for a given process comm name.
 * Returns MODE_BALANCE (default) if no rule matches. */
PowerMode perapp_lookup(const PerAppConfig *cfg, const char *comm);

/* Match one process using both its comm and executable command line while
 * preserving first-rule-wins ordering. Returns true when a rule matched. */
bool perapp_lookup_process(const PerAppConfig *cfg, const char *comm,
                           const char *cmdline, PowerMode *mode);

/* Look up the power mode for a given process by PID (reads /proc/[pid]/comm).
 * Returns MODE_BALANCE if comm cannot be read or no rule matches. */
PowerMode perapp_lookup_pid(const PerAppConfig *cfg, pid_t pid);

/* Free any resources held by perapp config. */
void perapp_free(PerAppConfig *cfg);

#endif /* UPERF_PERAPP_CONFIG_H */
