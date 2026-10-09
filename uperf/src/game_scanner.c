#define _GNU_SOURCE
#include "game_scanner.h"
#include "log.h"
#include "perapp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Known game patterns */
static const char *DEFAULT_PATTERNS[] = {
    "UnityMain", "GameThread", "RenderThread", "GLThread",
    "dolphin", "ppsspp", "retroarch", "wine", "proton", "miHoYo",
    "hoyoverse", "minecraft", "gameloft", "supercell", "niantic",
    "rovio", "ea.games", "playdead", "half-life", "steam_app_",
    "gta", "pubg", "fortnite", "callofduty", "genshin", "honkai",
    "arknights", "yuzu", "ryujinx",
    NULL
};

/* Internal game scanner state */
struct GameScanner {
    char *patterns[MAX_PATTERNS];
    int nr_patterns;
    GameProcess entries[MAX_GAMES];
    int nr_entries;
    char app_modes[MAX_GAMES][MAX_NAME_LEN]; /* per-app mode storage */
    PerAppConfig perapp;
};

static uint64_t read_process_start_time(pid_t pid) {
    char path[64];
    char buffer[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    if (!fgets(buffer, sizeof(buffer), file)) {
        fclose(file);
        return 0;
    }
    fclose(file);

    char *cursor = strrchr(buffer, ')');
    if (!cursor || cursor[1] != ' ') return 0;
    cursor += 2;
    if (cursor[0] == '\0' || cursor[1] != ' ') return 0;
    cursor += 2;
    for (int field = 4; field <= 22; field++) {
        while (*cursor == ' ') cursor++;
        if (*cursor == '\0') return 0;
        char *end = NULL;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (end == cursor) return 0;
        if (field == 22) return (uint64_t)value;
        cursor = end;
    }
    return 0;
}

static void copy_truncated(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    size_t n = src ? strnlen(src, dst_size - 1) : 0;
    if (n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool matches_pattern(GameScanner *gs, const char *text) {
    if (!text) return false;
    for (int i = 0; i < gs->nr_patterns; i++) {
        if (strcasestr(text, gs->patterns[i])) return true;
    }
    return false;
}

static bool rule_line_matches_app(const char *line, const char *app) {
    if (!line || !app) return false;
    char copy[512];
    copy_truncated(copy, sizeof(copy), line);
    char *start = copy;
    while (isspace((unsigned char)*start)) start++;
    if (*start == '\0' || *start == '#') return false;
    char *end = start;
    while (*end && *end != ':' && !isspace((unsigned char)*end)) end++;
    *end = '\0';
    return strcasecmp(start, app) == 0;
}

bool game_scanner_match(const char *comm, const char *cmdline) {
    /* Default match without scanner instance */
    GameScanner dummy;
    dummy.nr_patterns = 0;
    for (int i = 0; DEFAULT_PATTERNS[i]; i++) {
        dummy.patterns[dummy.nr_patterns++] = (char *)DEFAULT_PATTERNS[i];
        if (dummy.nr_patterns >= MAX_PATTERNS) break;
    }
    bool r = matches_pattern(&dummy, comm) || matches_pattern(&dummy, cmdline);
    return r;
}

GameScanner *game_scanner_new(void) {
    GameScanner *gs = calloc(1, sizeof(*gs));
    if (!gs) return NULL;

    /* Load default patterns */
    for (int i = 0; DEFAULT_PATTERNS[i]; i++) {
        if (gs->nr_patterns >= MAX_PATTERNS) break;
        gs->patterns[gs->nr_patterns++] = strdup(DEFAULT_PATTERNS[i]);
    }

    log_debug("GameScanner created with %d patterns", gs->nr_patterns);
    return gs;
}

void game_scanner_free(GameScanner *gs) {
    if (!gs) return;
    for (int i = 0; i < gs->nr_patterns; i++) {
        free(gs->patterns[i]);
    }
    perapp_free(&gs->perapp);
    free(gs);
    log_debug("GameScanner destroyed");
}

int game_scanner_add_pattern(GameScanner *gs, const char *pattern) {
    if (!gs || !pattern) return -1;
    if (gs->nr_patterns >= MAX_PATTERNS) return -1;
    gs->patterns[gs->nr_patterns++] = strdup(pattern);
    return 0;
}

int game_scanner_pattern_count(const GameScanner *gs) {
    return gs ? gs->nr_patterns : 0;
}

int game_scanner_scan(GameScanner *gs) {
    if (!gs) return -1;

    DIR *proc = opendir("/proc");
    if (!proc) {
        log_error("game_scanner: cannot open /proc: %s", strerror(errno));
        return -1;
    }

    struct dirent *ent;
    int count = 0;

    while ((ent = readdir(proc)) && count < MAX_GAMES) {
        if (!isdigit((unsigned char)ent->d_name[0])) continue;

        pid_t pid = atoi(ent->d_name);
        if (pid < 2) continue;

        /* Read comm */
        char comm_path[64];
        char comm[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
        FILE *fp = fopen(comm_path, "r");
        if (!fp) continue;
        if (!fgets(comm, sizeof(comm), fp)) {
            fclose(fp);
            continue;
        }
        comm[strcspn(comm, "\n")] = '\0';
        fclose(fp);

        /* Read cmdline */
        char cmdline[512] = {0};
        bool command_matches = false;
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/cmdline", pid);
        fp = fopen(comm_path, "r");
        if (fp) {
            size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, fp);
            /* Only the executable/process-name token participates in game
             * detection. Arguments can contain ordinary words or even this
             * program's own game-pattern configuration. */
            command_matches = matches_pattern(gs, cmdline);
            for (size_t i = 0; i < n; i++)
                if (cmdline[i] == '\0') cmdline[i] = ' ';
            fclose(fp);
        }

        PowerMode configured_mode = MODE_BALANCE;
        bool configured = perapp_lookup_process(
            &gs->perapp, comm, cmdline, &configured_mode);
        if (matches_pattern(gs, comm) || command_matches ||
            (configured && configured_mode != MODE_BALANCE)) {
            uint64_t start_time = read_process_start_time(pid);
            if (start_time == 0) continue;
            GameProcess *p = &gs->entries[count];
            p->pid = pid;
            p->start_time = start_time;
            copy_truncated(p->comm, sizeof(p->comm), comm);
            copy_truncated(p->cmdline, sizeof(p->cmdline), cmdline);
            p->package[0] = '\0';
            p->is_game = true;
            PowerMode mode = configured ? configured_mode : MODE_BALANCE;
            const char *mode_name = mode == MODE_POWERSAVE ? "powersave" :
                mode == MODE_PERFORMANCE ? "performance" :
                mode == MODE_FAST ? "fast" : "balance";
            copy_truncated(gs->app_modes[count], MAX_NAME_LEN, mode_name);
            count++;
        }
    }

    closedir(proc);
    gs->nr_entries = count;
    log_debug("game_scanner: found %d game process(es)", count);
    return count;
}

int game_scanner_get_results(GameScanner *gs, GameProcess *out, int max_entries) {
    if (!gs || !out) return -1;
    int n = gs->nr_entries < max_entries ? gs->nr_entries : max_entries;
    for (int i = 0; i < n; i++) {
        out[i] = gs->entries[i];
    }
    return n;
}

int game_scanner_perapp_scan(GameScanner *gs, const char *perapp_file) {
    if (!gs || !perapp_file) return -1;
    return perapp_load(&gs->perapp, perapp_file);
}

const char *game_scanner_get_app_mode(const GameScanner *gs, const char *comm) {
    if (!gs || !comm) return "balance";
    for (int i = 0; i < gs->nr_entries; i++) {
        if (strcmp(gs->entries[i].comm, comm) == 0 && gs->app_modes[i][0] != '\0') {
            return gs->app_modes[i];
        }
    }
    return "balance";
}

int game_scanner_set_app_mode(GameScanner *gs, const char *perapp_file,
                              const char *comm, const char *mode) {
    if (!gs || !perapp_file || !comm || !mode || comm[0] == '\0' ||
        strlen(comm) >= MAX_NAME_LEN || strpbrk(comm, " \t\r\n") ||
        (strcmp(mode, "balance") != 0 && strcmp(mode, "powersave") != 0 &&
         strcmp(mode, "performance") != 0 && strcmp(mode, "fast") != 0))
        return -1;

    char temp_path[MAX_PATH_LEN];
    int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX",
                           perapp_file);
    if (written < 0 || written >= (int)sizeof(temp_path)) return -1;
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        log_error("perapp: cannot create temporary file for %s: %s",
                  perapp_file, strerror(errno));
        return -1;
    }
    struct stat existing;
    mode_t mode_bits = stat(perapp_file, &existing) == 0
        ? existing.st_mode & 0777 : 0644;
    if (fchmod(fd, mode_bits) != 0) {
        close(fd);
        unlink(temp_path);
        return -1;
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        unlink(temp_path);
        return -1;
    }

    /* New rules go first because perapp matching intentionally uses the first
     * match. Existing comments and generic rules are retained below it. */
    fprintf(out, "%s %s\n", comm, mode);
    FILE *in = fopen(perapp_file, "r");
    if (in) {
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            if (!rule_line_matches_app(line, comm))
                fputs(line, out);
        }
        fclose(in);
    }
    bool write_ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
    if (fclose(out) != 0) write_ok = false;
    if (!write_ok || rename(temp_path, perapp_file) != 0) {
        unlink(temp_path);
        log_error("perapp: cannot replace %s: %s", perapp_file,
                  strerror(errno));
        return -1;
    }

    if (perapp_load(&gs->perapp, perapp_file) < 0) return -1;
    for (int i = 0; i < gs->nr_entries; i++) {
        if (strcmp(gs->entries[i].comm, comm) == 0)
            copy_truncated(gs->app_modes[i], MAX_NAME_LEN, mode);
    }
    return 0;
}
