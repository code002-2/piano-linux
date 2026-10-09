#define _GNU_SOURCE
#include "perapp_config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/* Trim leading/trailing whitespace in-place, return pointer to first non-space */
static char *trim(char *s) {
    while (*s != '\0' && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

/* Parse a mode string into PowerMode enum */
static PowerMode parse_mode_string(const char *s) {
    if (!s) return MODE_BALANCE;
    if (strcmp(s, "balance") == 0)       return MODE_BALANCE;
    if (strcmp(s, "powersave") == 0)     return MODE_POWERSAVE;
    if (strcmp(s, "performance") == 0)   return MODE_PERFORMANCE;
    if (strcmp(s, "fast") == 0)          return MODE_FAST;
    return MODE_BALANCE;
}

int perapp_load(PerAppConfig *cfg, const char *path) {
    if (!cfg || !path) return -1;

    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->file_path, path, MAX_PATH_LEN - 1);
    cfg->file_path[MAX_PATH_LEN - 1] = '\0';

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT) {
            log_error("perapp: cannot open '%s': %s", path, strerror(errno));
        }
        return 0;  /* File not found is OK — use defaults */
    }

    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp) && cfg->nr_rules < PERAPP_MAX_RULES) {
        lineno++;
        char *trimmed = trim(line);

        /* Skip empty lines and comments */
        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;

        /* Support both "pattern:mode" and "pattern mode" formats */
        char *sep = strchr(trimmed, ':');
        char *space = strchr(trimmed, ' ');

        char *pattern, *mode_str;
        if (sep && (!space || sep < space)) {
            /* Colon-separated */
            *sep = '\0';
            pattern = trim(trimmed);
            mode_str = trim(sep + 1);
        } else if (space) {
            /* Space-separated */
            *space = '\0';
            pattern = trim(trimmed);
            mode_str = trim(space + 1);
        } else {
            log_warn("perapp: invalid format at line %d: '%s'", lineno, trimmed);
            continue;
        }

        if (strlen(pattern) == 0 || strlen(mode_str) == 0) {
            log_warn("perapp: empty pattern or mode at line %d", lineno);
            continue;
        }

        strncpy(cfg->rules[cfg->nr_rules].pattern, pattern, MAX_NAME_LEN - 1);
        cfg->rules[cfg->nr_rules].pattern[MAX_NAME_LEN - 1] = '\0';
        cfg->rules[cfg->nr_rules].mode = parse_mode_string(mode_str);
        cfg->nr_rules++;

        log_debug("perapp: rule[%d] '%s' → %s",
                  cfg->nr_rules - 1, pattern, mode_str);
    }

    fclose(fp);
    log_info("perapp: loaded %d rule(s) from '%s'", cfg->nr_rules, path);
    return 0;
}

static bool pattern_matches(const char *pattern, const char *text) {
    if (!pattern || !text || text[0] == '\0') return false;
    if (strcmp(pattern, ".*") == 0 || strcmp(pattern, "*") == 0)
        return true;
    return strcasestr(text, pattern) != NULL;
}

bool perapp_lookup_process(const PerAppConfig *cfg, const char *comm,
                           const char *cmdline, PowerMode *mode) {
    if (!cfg || !mode || cfg->nr_rules == 0) return false;

    /* Check each rule — first match wins (more specific patterns should be listed first) */
    for (int i = 0; i < cfg->nr_rules; i++) {
        if (pattern_matches(cfg->rules[i].pattern, comm) ||
            pattern_matches(cfg->rules[i].pattern, cmdline)) {
            *mode = cfg->rules[i].mode;
            log_debug("perapp: process='%s' matched rule '%s' → mode %d",
                      comm ? comm : "", cfg->rules[i].pattern, *mode);
            return true;
        }
    }

    return false;
}

PowerMode perapp_lookup(const PerAppConfig *cfg, const char *comm) {
    PowerMode mode = MODE_BALANCE;
    perapp_lookup_process(cfg, comm, NULL, &mode);
    return mode;
}

PowerMode perapp_lookup_pid(const PerAppConfig *cfg, pid_t pid) {
    if (!cfg || pid <= 0)
        return MODE_BALANCE;

    char comm_path[MAX_PATH_LEN];
    snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);

    FILE *fp = fopen(comm_path, "r");
    if (!fp) return MODE_BALANCE;

    char comm[64];
    if (fgets(comm, sizeof(comm), fp)) {
        comm[strcspn(comm, "\n")] = '\0';
        fclose(fp);
        return perapp_lookup(cfg, comm);
    }

    fclose(fp);
    return MODE_BALANCE;
}

void perapp_free(PerAppConfig *cfg) {
    if (!cfg) return;
    log_debug("perapp: freed %d rule(s)", cfg->nr_rules);
    memset(cfg, 0, sizeof(*cfg));
}
