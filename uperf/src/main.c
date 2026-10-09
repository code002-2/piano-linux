#define _GNU_SOURCE
#include "log.h"
#include "config.h"
#include "state_machine.h"
#include "input_monitor.h"
#include "sysfs_writer.h"
#include "heavyload_detector.h"
#include "power_model.h"
#include "frequency_controller.h"
#include "frequency_state.h"
#include "game_scanner.h"
#include "task_scheduler.h"
#include "cgroup_manager.h"
#include "thermal.h"
#include "dbus_interface.h"
#include "perapp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <gio/gio.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Global state */
static Config g_config;
static StateMachine *g_sm = NULL;
static InputMonitor *g_im = NULL;
static SysfsWriter *g_writer = NULL;
static HeavyLoadDetector *g_detector = NULL;
static GameScanner *g_scanner = NULL;
static TaskScheduler *g_task_scheduler = NULL;
static CgroupManager *g_cgroup_manager = NULL;
static DbusManager *g_dbus = NULL;
static ThermalManager *g_thermal = NULL;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload_config = 0;
static char g_config_path[MAX_PATH_LEN];

typedef struct {
    char min_path[MAX_PATH_LEN];
    char max_path[MAX_PATH_LEN];
    char current_path[MAX_PATH_LEN];
    char original_min[32];
    char original_max[32];
    gint64 hardware_min_hz;
    gint64 hardware_max_hz;
    gint64 unit_hz;
    uint64_t cpu_mask;
    bool manual_active;
    bool modified;
    bool automatic_disabled;
    bool valid;
} ManualFreqTarget;

static ManualFreqTarget g_manual_cpu[MAX_CLUSTERS];
static ManualFreqTarget g_manual_gpu;
static int g_nr_cpu_targets;
static bool g_automatic_frequency_enabled;
static PowerMode g_requested_mode = MODE_BALANCE;
static PowerMode g_last_detected_app_mode = MODE_NUM;
static GameProcess g_game_results[MAX_GAMES];
static int g_nr_game_results;

#define FREQUENCY_STATE_FILE "/run/uperf-linux/frequency-state"
#define SCHEDULER_STATE_FILE "/run/uperf-linux/scheduler-state"
#define CGROUP_STATE_FILE "/run/uperf-linux/cgroup-state"

static gint64 parse_sysfs_number(const char *value) {
    if (!value) return -1;
    char *end = NULL;
    errno = 0;
    gint64 result = g_ascii_strtoll(value, &end, 10);
    return errno == 0 && end != value ? result : -1;
}

static uint64_t parse_cpu_list(const char *value) {
    if (!value) return 0;
    const char *cursor = value;
    uint64_t mask = 0;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') cursor++;
        if (!*cursor) break;

        char *end = NULL;
        errno = 0;
        long first = strtol(cursor, &end, 10);
        if (errno != 0 || end == cursor || first < 0 || first >= 64)
            return 0;
        long last = first;
        cursor = end;
        if (*cursor == '-') {
            cursor++;
            errno = 0;
            last = strtol(cursor, &end, 10);
            if (errno != 0 || end == cursor || last < first || last >= 64)
                return 0;
            cursor = end;
        }
        for (long cpu = first; cpu <= last; cpu++)
            mask |= UINT64_C(1) << cpu;
        if (*cursor && *cursor != ' ' && *cursor != '\t' && *cursor != ',')
            return 0;
    }
    return mask;
}

static bool parse_frequency_range(const char *value, gint64 *minimum,
                                  gint64 *maximum) {
    if (!value || !minimum || !maximum) return false;
    gint64 low = G_MAXINT64;
    gint64 high = 0;
    const char *cursor = value;
    while (*cursor) {
        while (g_ascii_isspace(*cursor)) cursor++;
        if (!*cursor) break;
        char *end = NULL;
        errno = 0;
        gint64 frequency = g_ascii_strtoll(cursor, &end, 10);
        if (errno != 0 || end == cursor || frequency <= 0) return false;
        if (frequency < low) low = frequency;
        if (frequency > high) high = frequency;
        cursor = end;
    }
    if (low == G_MAXINT64 || high < low) return false;
    *minimum = low;
    *maximum = high;
    return true;
}

static int cpu_mask_count(uint64_t mask) {
    int count = 0;
    while (mask) {
        count += (int)(mask & 1u);
        mask >>= 1;
    }
    return count;
}

static bool initialize_manual_target(ManualFreqTarget *target,
                                     const char *min_path,
                                     const char *max_path,
                                     const char *current_path,
                                     gint64 hardware_min_hz,
                                     gint64 hardware_max_hz,
                                     gint64 unit_hz) {
    char *current_min = sysfs_reader_read(min_path);
    char *current_max = sysfs_reader_read(max_path);
    if (!current_min || !current_max || hardware_min_hz <= 0 ||
        hardware_max_hz < hardware_min_hz) {
        free(current_min);
        free(current_max);
        return false;
    }

    snprintf(target->min_path, sizeof(target->min_path), "%s", min_path);
    snprintf(target->max_path, sizeof(target->max_path), "%s", max_path);
    if (current_path)
        snprintf(target->current_path, sizeof(target->current_path), "%s",
                 current_path);
    snprintf(target->original_min, sizeof(target->original_min), "%s",
             current_min);
    snprintf(target->original_max, sizeof(target->original_max), "%s",
             current_max);
    target->hardware_min_hz = hardware_min_hz;
    target->hardware_max_hz = hardware_max_hz;
    target->unit_hz = unit_hz;
    target->valid = true;
    free(current_min);
    free(current_max);
    return true;
}

static bool snapshot_values_fit_target(const FrequencyStateEntry *entry,
                                       const ManualFreqTarget *target) {
    gint64 minimum = parse_sysfs_number(entry->original_min);
    gint64 maximum = parse_sysfs_number(entry->original_max);
    if (minimum <= 0 || maximum < minimum || target->unit_hz <= 0 ||
        minimum > G_MAXINT64 / target->unit_hz ||
        maximum > G_MAXINT64 / target->unit_hz)
        return false;
    minimum *= target->unit_hz;
    maximum *= target->unit_hz;
    return minimum >= target->hardware_min_hz &&
           maximum <= target->hardware_max_hz;
}

static void target_to_state_entry(const ManualFreqTarget *target,
                                  FrequencyStateEntry *entry) {
    snprintf(entry->min_path, sizeof(entry->min_path), "%s",
             target->min_path);
    snprintf(entry->max_path, sizeof(entry->max_path), "%s",
             target->max_path);
    snprintf(entry->original_min, sizeof(entry->original_min), "%s",
             target->original_min);
    snprintf(entry->original_max, sizeof(entry->original_max), "%s",
             target->original_max);
}

static bool prepare_frequency_state(void) {
    if (geteuid() != 0) return true;

    FrequencyStateEntry saved[FREQUENCY_STATE_MAX_ENTRIES] = {0};
    size_t saved_count = 0;
    int load_result = frequency_state_load(
        FREQUENCY_STATE_FILE, saved, FREQUENCY_STATE_MAX_ENTRIES,
        &saved_count);
    if (load_result == FREQUENCY_STATE_ERROR) {
        log_error("Frequency state file is invalid; automatic control disabled");
        return false;
    }

    ManualFreqTarget *targets[FREQUENCY_STATE_MAX_ENTRIES] = {0};
    size_t target_count = 0;
    for (int i = 0; i < g_nr_cpu_targets; i++) {
        if (g_manual_cpu[i].valid)
            targets[target_count++] = &g_manual_cpu[i];
    }
    if (g_manual_gpu.valid) targets[target_count++] = &g_manual_gpu;
    if (target_count == 0) return false;

    if (load_result == FREQUENCY_STATE_OK) {
        for (size_t i = 0; i < target_count; i++) {
            ManualFreqTarget *target = targets[i];
            const FrequencyStateEntry *entry = frequency_state_find(
                saved, saved_count, target->min_path, target->max_path);
            if (!entry) continue; /* New target introduced by config reload. */
            if (!snapshot_values_fit_target(entry, target)) {
                log_error("Saved frequency range is invalid for %s",
                          target->max_path);
                return false;
            }
            /* The values captured during discovery describe the limits left
             * behind by the previous (possibly killed) process.  Remember
             * that they differ before replacing them with the true originals
             * from the recovery snapshot, so an immediate clean shutdown
             * still restores those originals. */
            target->modified =
                strcmp(target->original_min, entry->original_min) != 0 ||
                strcmp(target->original_max, entry->original_max) != 0;
            snprintf(target->original_min, sizeof(target->original_min), "%s",
                     entry->original_min);
            snprintf(target->original_max, sizeof(target->original_max), "%s",
                     entry->original_max);
        }
    }

    FrequencyStateEntry current[FREQUENCY_STATE_MAX_ENTRIES] = {0};
    for (size_t i = 0; i < target_count; i++)
        target_to_state_entry(targets[i], &current[i]);
    if (frequency_state_save(FREQUENCY_STATE_FILE, current, target_count) !=
        FREQUENCY_STATE_OK) {
        log_error("Cannot persist original frequency limits; automatic "
                  "control disabled");
        return false;
    }
    return true;
}

static void discover_manual_frequency_targets(void) {
    memset(g_manual_cpu, 0, sizeof(g_manual_cpu));
    memset(&g_manual_gpu, 0, sizeof(g_manual_gpu));
    g_nr_cpu_targets = 0;
    g_automatic_frequency_enabled = false;
    ManualFreqTarget candidates[8] = {0};
    int nr_candidates = 0;
    DIR *dir = opendir("/sys/devices/system/cpu/cpufreq");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && nr_candidates < 8) {
            if (strncmp(entry->d_name, "policy", 6) != 0) continue;
            char *end = NULL;
            long policy_id = strtol(entry->d_name + 6, &end, 10);
            if (end == entry->d_name + 6 || *end != '\0' ||
                policy_id < 0 || policy_id > 4096)
                continue;
            char min_path[MAX_PATH_LEN], max_path[MAX_PATH_LEN];
            char current_path[MAX_PATH_LEN], related_path[MAX_PATH_LEN];
            char hw_min_path[MAX_PATH_LEN], hw_max_path[MAX_PATH_LEN];
            snprintf(min_path, sizeof(min_path),
                     "/sys/devices/system/cpu/cpufreq/policy%ld/scaling_min_freq",
                     policy_id);
            snprintf(max_path, sizeof(max_path),
                     "/sys/devices/system/cpu/cpufreq/policy%ld/scaling_max_freq",
                     policy_id);
            snprintf(current_path, sizeof(current_path),
                     "/sys/devices/system/cpu/cpufreq/policy%ld/scaling_cur_freq",
                     policy_id);
            snprintf(related_path, sizeof(related_path),
                     "/sys/devices/system/cpu/cpufreq/policy%ld/related_cpus",
                     policy_id);
            snprintf(hw_min_path, sizeof(hw_min_path),
                     "/sys/devices/system/cpu/cpufreq/policy%ld/cpuinfo_min_freq",
                     policy_id);
            snprintf(hw_max_path, sizeof(hw_max_path),
                     "/sys/devices/system/cpu/cpufreq/policy%ld/cpuinfo_max_freq",
                     policy_id);
            char *hw_min = sysfs_reader_read(hw_min_path);
            char *hw_max = sysfs_reader_read(hw_max_path);
            gint64 min_hz = parse_sysfs_number(hw_min) * 1000;
            gint64 max_hz = parse_sysfs_number(hw_max) * 1000;
            free(hw_min);
            free(hw_max);
            if (initialize_manual_target(&candidates[nr_candidates],
                                         min_path, max_path, current_path,
                                         min_hz, max_hz,
                                         1000))
            {
                char *related = sysfs_reader_read(related_path);
                candidates[nr_candidates].cpu_mask = parse_cpu_list(related);
                free(related);
                if (candidates[nr_candidates].cpu_mask != 0)
                    nr_candidates++;
                else
                    memset(&candidates[nr_candidates], 0,
                           sizeof(candidates[nr_candidates]));
            }
        }
        closedir(dir);
    }
    /* Bind each configured model to its exact cpufreq policy mask.  Frequency
     * ordering is not a topology contract and is ambiguous on other SoCs. */
    g_nr_cpu_targets = g_config.cpu.nr_clusters;
    bool topology_matches = nr_candidates == g_config.cpu.nr_clusters;
    for (int model = 0; model < g_nr_cpu_targets; model++) {
        uint64_t expected = g_config.cpu.power_model[model].cpu_mask;
        int match = -1;
        for (int candidate = 0; candidate < nr_candidates; candidate++) {
            if (candidates[candidate].cpu_mask == expected) {
                match = candidate;
                break;
            }
        }
        if (match < 0) {
            topology_matches = false;
            log_error("No cpufreq policy matches powerModel[%d] mask 0x%" PRIx64,
                      model, expected);
            continue;
        }
        g_manual_cpu[model] = candidates[match];
    }

    const char *gpu_min_path = NULL;
    const char *gpu_max_path = NULL;
    for (int i = 0; i < g_config.sysfs.nr_knobs; i++) {
        if (strcmp(g_config.sysfs.knobs[i].name, "gpuMinFreq") == 0)
            gpu_min_path = g_config.sysfs.knobs[i].path;
        else if (strcmp(g_config.sysfs.knobs[i].name, "gpuMaxFreq") == 0)
            gpu_max_path = g_config.sysfs.knobs[i].path;
    }
    if (gpu_min_path && gpu_max_path) {
        char *gpu_min = sysfs_reader_read(gpu_min_path);
        char *gpu_max = sysfs_reader_read(gpu_max_path);
        gint64 min_hz = parse_sysfs_number(gpu_min);
        gint64 max_hz = parse_sysfs_number(gpu_max);
        free(gpu_min);
        free(gpu_max);
        char available_path[MAX_PATH_LEN];
        const char *last_slash = strrchr(gpu_min_path, '/');
        int prefix_len = last_slash ? (int)(last_slash - gpu_min_path) : -1;
        if (prefix_len > 0 && prefix_len < (int)sizeof(available_path) - 24) {
            snprintf(available_path, sizeof(available_path), "%.*s/%s",
                     prefix_len, gpu_min_path, "available_frequencies");
            char *available = sysfs_reader_read(available_path);
            parse_frequency_range(available, &min_hz, &max_hz);
            free(available);
        }
        initialize_manual_target(&g_manual_gpu, gpu_min_path, gpu_max_path,
                                 NULL, min_hz, max_hz, 1);
    }

    bool frequency_state_ready = prepare_frequency_state();

    for (int i = 0; topology_matches && i < g_nr_cpu_targets; i++) {
        if (!g_manual_cpu[i].valid ||
            g_manual_cpu[i].cpu_mask != g_config.cpu.power_model[i].cpu_mask ||
            cpu_mask_count(g_manual_cpu[i].cpu_mask) !=
            g_config.cpu.power_model[i].nr_cores)
            topology_matches = false;
    }
    g_automatic_frequency_enabled = geteuid() == 0 && g_config.cpu.enable &&
        topology_matches && frequency_state_ready;
    if (!topology_matches)
        log_warn("Automatic frequency control disabled: detected cpufreq "
                 "topology does not match the configured power model");
    else if (geteuid() != 0)
        log_info("Automatic frequency control disabled for unprivileged run");

    log_info("Frequency targets: CPU=%d GPU=%s automatic=%s", nr_candidates,
             g_manual_gpu.valid ? "available" : "unavailable",
             g_automatic_frequency_enabled ? "enabled" : "disabled");
}

static int write_if_changed(const char *path, const char *current,
                            const char *desired) {
    if (current && strcmp(current, desired) == 0) return 0;
    return sysfs_writer_write_raw(g_writer, path, desired);
}

static gboolean write_target_limits(ManualFreqTarget *target,
                                    gint64 minimum_hz,
                                    gint64 maximum_hz) {
    if (!g_writer || !target || !target->valid || target->unit_hz <= 0 ||
        minimum_hz < target->hardware_min_hz ||
        maximum_hz > target->hardware_max_hz || minimum_hz > maximum_hz)
        return FALSE;

    char desired_min[32], desired_max[32];
    snprintf(desired_min, sizeof(desired_min), "%" G_GINT64_FORMAT,
             minimum_hz / target->unit_hz);
    snprintf(desired_max, sizeof(desired_max), "%" G_GINT64_FORMAT,
             maximum_hz / target->unit_hz);

    char *current_min = sysfs_reader_read(target->min_path);
    char *current_max = sysfs_reader_read(target->max_path);
    if (!current_min || !current_max) {
        free(current_min);
        free(current_max);
        return FALSE;
    }
    if (strcmp(current_min, desired_min) == 0 &&
        strcmp(current_max, desired_max) == 0) {
        free(current_min);
        free(current_max);
        return TRUE;
    }

    gint64 current_min_value = parse_sysfs_number(current_min);
    gint64 desired_max_value = parse_sysfs_number(desired_max);
    int first_rc, second_rc;
    if (current_min_value >= 0 && desired_max_value < current_min_value) {
        first_rc = write_if_changed(target->min_path, current_min, desired_min);
        second_rc = first_rc == 0
            ? write_if_changed(target->max_path, current_max, desired_max) : -1;
    } else {
        first_rc = write_if_changed(target->max_path, current_max, desired_max);
        second_rc = first_rc == 0
            ? write_if_changed(target->min_path, current_min, desired_min) : -1;
    }

    if (first_rc != 0 || second_rc != 0) {
        /* Roll back to the limits that were in force before this transaction. */
        sysfs_writer_write_raw(g_writer, target->max_path, current_max);
        sysfs_writer_write_raw(g_writer, target->min_path, current_min);
        free(current_min);
        free(current_max);
        return FALSE;
    }

    target->modified = strcmp(desired_min, target->original_min) != 0 ||
                       strcmp(desired_max, target->original_max) != 0;
    free(current_min);
    free(current_max);
    return TRUE;
}

static gboolean restore_target(ManualFreqTarget *target) {
    if (!target || !target->valid) return FALSE;
    gint64 minimum = parse_sysfs_number(target->original_min);
    gint64 maximum = parse_sysfs_number(target->original_max);
    if (minimum < 0 || maximum < minimum) return FALSE;
    gboolean result = write_target_limits(target,
                                          minimum * target->unit_hz,
                                          maximum * target->unit_hz);
    if (result) {
        target->manual_active = false;
        target->modified = false;
        target->automatic_disabled = false;
    }
    return result;
}

static gboolean write_manual_target(ManualFreqTarget *target, gint64 freq_hz) {
    if (!target || !target->valid || freq_hz < 0) return FALSE;
    if (freq_hz == 0) return restore_target(target);
    if (freq_hz < target->hardware_min_hz ||
        freq_hz > target->hardware_max_hz) {
        log_warn("Manual freq %" G_GINT64_FORMAT
                 " Hz outside hardware range %" G_GINT64_FORMAT
                 "-%" G_GINT64_FORMAT " Hz",
                 freq_hz, target->hardware_min_hz, target->hardware_max_hz);
        return FALSE;
    }
    gboolean result = write_target_limits(target, freq_hz, freq_hz);
    if (result) target->manual_active = true;
    return result;
}

static gboolean dbus_manual_freq_handler(int cluster, gint64 freq_hz,
                                          void *user_data) {
    (void)user_data;
    if (freq_hz < 0) return FALSE;
    if (cluster == -1)
        return write_manual_target(&g_manual_gpu, freq_hz);
    if (cluster >= 0 && cluster < 3)
        return write_manual_target(&g_manual_cpu[cluster], freq_hz);
    if (cluster == 3) {
        for (int i = 0; i < g_nr_cpu_targets; i++) {
            if (freq_hz != 0 &&
                (freq_hz < g_manual_cpu[i].hardware_min_hz ||
                 freq_hz > g_manual_cpu[i].hardware_max_hz))
                return FALSE;
        }
        gboolean success = TRUE;
        int applied = 0;
        for (; applied < g_nr_cpu_targets; applied++) {
            if (!write_manual_target(&g_manual_cpu[applied], freq_hz)) {
                success = FALSE;
                break;
            }
        }
        if (!success) {
            while (--applied >= 0) restore_target(&g_manual_cpu[applied]);
        }
        return success;
    }
    return FALSE;
}

static gboolean restore_frequency_targets(void) {
    if (!g_writer) return TRUE;
    gboolean success = TRUE;
    for (int i = 0; i < g_nr_cpu_targets; i++) {
        if (g_manual_cpu[i].modified && !restore_target(&g_manual_cpu[i]))
            success = FALSE;
    }
    if (g_manual_gpu.modified && !restore_target(&g_manual_gpu))
        success = FALSE;
    return success;
}

static void apply_automatic_frequency_control(const float *loads,
                                              int nr_loads) {
    if (!g_automatic_frequency_enabled || !g_sm || !g_writer || !loads ||
        nr_loads <= 0)
        return;

    ActionParams params;
    state_machine_get_actions(g_sm, &params);
    float thermal = state_machine_get_thermal_reduction(g_sm);
    for (int cluster = 0; cluster < g_nr_cpu_targets; cluster++) {
        ManualFreqTarget *target = &g_manual_cpu[cluster];
        if (!target->valid || target->manual_active ||
            target->automatic_disabled)
            continue;

        float demand = frequency_controller_cluster_demand(
            loads, (size_t)nr_loads, target->cpu_mask);
        gint64 minimum_hz, maximum_hz;
        frequency_controller_compute_limits(
            &g_config.cpu.power_model[cluster], &params, cluster, demand,
            thermal, target->hardware_min_hz, target->hardware_max_hz,
            &minimum_hz, &maximum_hz);
        if (!write_target_limits(target, minimum_hz, maximum_hz)) {
            target->automatic_disabled = true;
            log_error("Automatic frequency control disabled for cluster %d "
                      "after a sysfs write failure", cluster);
        }
    }
}

/* Read /proc/<pid>/comm into buf (NUL-terminated, newline stripped).
 * Returns TRUE on success. */
static gboolean read_pid_comm(pid_t pid, char *buf, size_t len) {
    if (pid <= 0 || !buf || len == 0) return FALSE;
    char proc_path[MAX_PATH_LEN];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d/comm", pid);
    FILE *fp = fopen(proc_path, "r");
    if (!fp) return FALSE;
    gboolean ok = fgets(buf, (int)len, fp) != NULL;
    fclose(fp);
    if (!ok) return FALSE;
    buf[strcspn(buf, "\r\n")] = '\0';
    return TRUE;
}

static gboolean dbus_game_mode_handler(pid_t pid, const char *app_name,
                                        const char *mode, void *user_data) {
    (void)user_data;
    if (!g_scanner || pid <= 0 || !app_name || !mode) return FALSE;

    char comm[MAX_NAME_LEN];
    if (!read_pid_comm(pid, comm, sizeof(comm))) return FALSE;
    if (strcmp(comm, app_name) != 0) {
        log_warn("DBus SetGameMode PID/name mismatch: %d is '%s', not '%s'",
                 pid, comm, app_name);
        return FALSE;
    }

    return game_scanner_set_app_mode(g_scanner,
                                     g_config.switcher.perapp_file,
                                     comm, mode) == 0;
}

static gboolean dbus_active_pid_handler(pid_t pid, void *user_data) {
    (void)user_data;
    return g_task_scheduler &&
        task_scheduler_set_active_pid(g_task_scheduler, pid) == 0;
}

static const char *power_mode_to_string(PowerMode mode) {
    switch (mode) {
        case MODE_POWERSAVE: return "powersave";
        case MODE_PERFORMANCE: return "performance";
        case MODE_FAST: return "fast";
        default: return "balance";
    }
}

static void apply_power_mode(PowerMode mode, const char *source) {
    if (mode == MODE_FAST) mode = MODE_PERFORMANCE;
    if (!g_sm || mode < 0 || mode >= MODE_NUM) return;
    if (state_machine_get_mode(g_sm) == mode) return;
    state_machine_set_mode(g_sm, mode);
    if (g_dbus) dbus_manager_set_mode(g_dbus, power_mode_to_string(mode));
    log_info("Power mode changed to %s (%s)", power_mode_to_string(mode),
             source ? source : "unknown source");
}

/* DBus mode change handler */
static void dbus_mode_handler(const char *mode, void *ud) {
    (void)ud;
    if (!mode) return;
    PowerMode power_mode;
    if (strcmp(mode, "balance") == 0)
        power_mode = MODE_BALANCE;
    else if (strcmp(mode, "powersave") == 0)
        power_mode = MODE_POWERSAVE;
    else if (strcmp(mode, "performance") == 0)
        power_mode = MODE_PERFORMANCE;
    else
        return;
    g_requested_mode = power_mode;
    apply_power_mode(power_mode, "D-Bus request");
}
static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT) {
        g_running = 0;
    } else if (sig == SIGHUP) {
        g_reload_config = 1;
    }
}

/* Usage / help */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Userspace performance scheduler for Linux ARM64 gaming.\n"
        "Inspired by Uperf-Game-Turbo (Android).\n"
        "\n"
        "Options:\n"
        "  -c, --config FILE    Config file path (default: /etc/uperf-linux/config.json)\n"
        "  -l, --log-level N    Log level 0=debug 1=info 2=warn 3=error 4=fatal\n"
        "  -L, --log-file FILE  Log file path (default: stderr)\n"
        "  -j, --journald       Also log to systemd-journald\n"
        "  -h, --help           Show this help\n"
        "  -v, --version        Show version\n"
        "\n",
        prog);
}

static void print_version(void) {
    printf("uperf-linux v0.1.0\n");
    printf("SM8550 (Snapdragon 8 Gen 2) performance scheduler\n");
    printf("Built: %s %s\n", __DATE__, __TIME__);
}

/* Apply initial sysfs knobs from config */
static void apply_initial_state(void) {
    /* Startup must not force every policy to an out-of-range 9.9 GHz value.
     * Publish the initial mode; automatic actions and explicit overrides are
     * applied later through their validated paths. */
    log_info("Publishing initial balance mode");
    if (g_dbus) dbus_manager_set_mode(g_dbus, "balance");
}

static const char *scene_to_string(SceneState scene) {
    static const char *names[SCENE_NUM_STATES] = {
        "idle", "touch", "trigger", "gesture", "junk", "switch", "boost"
    };
    return scene >= 0 && scene < SCENE_NUM_STATES ? names[scene] : "unknown";
}

static InputMonitor *create_input_monitor(const Config *config) {
    if (!config || !config->input.enable) return NULL;
    InputMonitor *monitor = input_monitor_new(
        config->input.swipe_thd,
        config->input.gesture_thd_x,
        config->input.gesture_thd_y,
        config->input.gesture_delay_time,
        config->input.screen_width > 0 ? config->input.screen_width : 1080,
        config->input.screen_height > 0 ? config->input.screen_height : 2400);
    if (!monitor) return NULL;
    input_monitor_discover_devices(monitor);
    if (input_monitor_device_count(monitor) == 0) {
        log_warn("No touchscreen devices found — input monitoring disabled");
        input_monitor_free(monitor);
        return NULL;
    }
    return monitor;
}

static ThermalManager *create_thermal_manager(const Config *config) {
    if (!config || !config->thermal.enable) return NULL;
    ThermalPolicy policy = {
        .warn_temp = config->thermal.warn_temp,
        .throttle_temp = config->thermal.throttle_temp,
        .critical_temp = config->thermal.critical_temp,
        .recovery_temp = config->thermal.recovery_temp,
    };
    ThermalManager *manager = thermal_manager_new(&policy);
    if (!manager) return NULL;
    int zones = thermal_manager_discover_zones(manager);
    if (zones > 0)
        log_info("Thermal manager initialized with %d zone(s)", zones);
    else
        log_warn("No thermal zones discovered — thermal throttling disabled");
    return manager;
}

static gboolean reload_configuration(void) {
    Config next_config;
    if (config_load(&next_config, g_config_path) < 0 ||
        config_validate(&next_config) < 0) {
        log_error("Configuration reload rejected; keeping active config");
        return FALSE;
    }

    StateMachine *next_sm = state_machine_new(&next_config);
    SysfsWriter *next_writer = sysfs_writer_new(&next_config, 1000000);
    if (!next_sm || !next_writer) {
        state_machine_free(next_sm);
        sysfs_writer_free(next_writer);
        config_free(&next_config);
        log_error("Configuration reload failed while creating core components");
        return FALSE;
    }

    PowerMode active_mode = state_machine_get_mode(g_sm);
    state_machine_set_mode(next_sm, active_mode);
    if (g_detector && heavyload_detector_is_heavy(g_detector))
        state_machine_feed_event(next_sm, EVT_HEAVY_LOAD_START);

    InputMonitor *next_input = create_input_monitor(&next_config);
    ThermalManager *next_thermal = create_thermal_manager(&next_config);
    pid_t active_pid =
        task_scheduler_get_requested_active_pid(g_task_scheduler);

    /* Release all limits using the writer and originals belonging to the old
     * configuration before swapping the live component set. */
    if (!restore_frequency_targets()) {
        thermal_manager_free(next_thermal);
        input_monitor_free(next_input);
        sysfs_writer_free(next_writer);
        state_machine_free(next_sm);
        config_free(&next_config);
        log_error("Configuration reload aborted: cannot restore active "
                  "frequency limits");
        return FALSE;
    }
    StateMachine *old_sm = g_sm;
    SysfsWriter *old_writer = g_writer;
    InputMonitor *old_input = g_im;
    ThermalManager *old_thermal = g_thermal;

    /* Scheduling state is tied to parsed rules. Restore the old policy before
     * atomically installing managers built from the new configuration. */
    cgroup_manager_free(g_cgroup_manager);
    g_cgroup_manager = NULL;
    task_scheduler_free(g_task_scheduler);
    g_task_scheduler = NULL;

    g_config = next_config;
    g_sm = next_sm;
    g_writer = next_writer;
    g_im = next_input;
    g_thermal = next_thermal;
    g_task_scheduler = task_scheduler_new(&g_config, SCHEDULER_STATE_FILE);
    g_cgroup_manager = cgroup_manager_new(&g_config, CGROUP_STATE_FILE);
    if (!g_task_scheduler || !g_cgroup_manager ||
        (g_config.cgroup.enable &&
         !cgroup_manager_is_available(g_cgroup_manager))) {
        log_fatal("Configuration reload could not initialize required "
                  "sched/cgroup managers; shutting down safely");
        g_running = 0;
    } else if (active_pid > 0) {
        task_scheduler_set_active_pid(g_task_scheduler, active_pid);
    }
    discover_manual_frequency_targets();

    if (g_scanner) {
        game_scanner_perapp_scan(g_scanner, g_config.switcher.perapp_file);
        game_scanner_scan(g_scanner);
        g_nr_game_results = game_scanner_get_results(
            g_scanner, g_game_results, MAX_GAMES);
    }
    if (g_dbus) {
        dbus_manager_set_scene(g_dbus,
            scene_to_string(state_machine_get_scene(g_sm)));
    }

    thermal_manager_free(old_thermal);
    input_monitor_free(old_input);
    sysfs_writer_free(old_writer);
    state_machine_free(old_sm);
    log_info("Configuration reloaded successfully from %s", g_config_path);
    return TRUE;
}

static gboolean dbus_reload_handler(void *user_data) {
    (void)user_data;
    return reload_configuration();
}

/* Main event loop */
static int event_loop(void) {
    log_info("Starting main event loop...");

    int poll_interval_ms = 50;  /* Base poll interval */
    bool previous_heavy = false;
    SceneState published_scene = state_machine_get_scene(g_sm);
    SceneState scheduling_scene = published_scene;
    gint64 last_policy_update_us = 0;
    gint64 last_sched_error_log_us = 0;
    gint64 last_cgroup_error_log_us = 0;

    while (g_running) {
        /* Dispatch D-Bus calls and GLib timers from the polling loop. */
        while (g_main_context_iteration(NULL, FALSE)) {}

        /* Handle config reload (from SIGHUP) */
        if (g_reload_config) {
            g_reload_config = 0;
            log_info("SIGHUP received — reloading config...");
            reload_configuration();
        }

        float *cpu_loads = NULL;
        int nr_cpu_loads = 0;

        /* Sample heavy load */
        if (g_detector) {
            heavyload_detector_sample(g_detector);
            cpu_loads = heavyload_detector_get_cpu_loads(g_detector,
                                                         &nr_cpu_loads);

            /* Update DBus with current stats */
            if (g_dbus) {
                double freqs[MAX_CLUSTERS] = {0};
                for (int cluster = 0; cluster < g_nr_cpu_targets; cluster++) {
                    char *val = sysfs_reader_read(
                        g_manual_cpu[cluster].current_path);
                    if (val) {
                        freqs[cluster] = atof(val) *
                            (double)g_manual_cpu[cluster].unit_hz / 1000000.0;
                        free(val);
                    }
                }
                dbus_manager_update_frequencies(g_dbus, freqs,
                                                g_nr_cpu_targets);
                if (cpu_loads && nr_cpu_loads > 0) {
                    double *loads = calloc((size_t)nr_cpu_loads,
                                           sizeof(*loads));
                    if (loads) {
                        for (int cpu = 0; cpu < nr_cpu_loads; cpu++)
                            loads[cpu] = cpu_loads[cpu];
                        dbus_manager_update_loads(g_dbus, loads, nr_cpu_loads);
                        free(loads);
                    }
                }
                bool heavy = heavyload_detector_is_heavy(g_detector);
                dbus_manager_set_heavy_load(g_dbus,
                                            heavy ? TRUE : FALSE);
            }

            /* Thermal monitoring — adjust behavior based on temperature */
            if (g_thermal) {
                int nr_zones = thermal_manager_zone_count(g_thermal);
                if (nr_zones > 0) {
                    ThermalState tstate = thermal_manager_update(g_thermal);
                    float reduction = thermal_manager_get_reduction_factor(g_thermal);
                    float old_reduction =
                        state_machine_get_thermal_reduction(g_sm);

                    if (reduction > 0.0f) {
                        /* Apply reduction to state machine for frequency capping */
                        state_machine_apply_thermal_reduction(g_sm, reduction);
                        if (reduction != old_reduction) {
                            log_warn("Thermal %s: reducing performance by %.0f%% "
                                     "(max temp: %d.%03d°C)",
                                     thermal_state_to_string(tstate),
                                     reduction * 100.0f,
                                     thermal_manager_get_max_temp(g_thermal) / 1000,
                                     thermal_manager_get_max_temp(g_thermal) % 1000);
                        }
                        /* Update DBus with thermal state for GUI */
                        if (g_dbus) {
                            dbus_manager_set_thermal_state(g_dbus,
                                thermal_manager_get_max_temp(g_thermal),
                                thermal_state_to_string(tstate));
                        }
                    } else {
                        /* Reset thermal reduction when back to normal */
                        state_machine_apply_thermal_reduction(g_sm, 0.0f);
                        if (g_dbus) {
                            dbus_manager_set_thermal_state(g_dbus,
                                thermal_manager_get_max_temp(g_thermal),
                                "normal");
                        }
                    }
                }
            }

            /* Feed only detector edges; repeatedly feeding START traps the
             * state machine in boost and suppresses its exit transition. */
            bool heavy = heavyload_detector_is_heavy(g_detector);
            if (g_sm && heavy != previous_heavy) {
                state_machine_feed_event(g_sm, heavy ? EVT_HEAVY_LOAD_START
                                                     : EVT_HEAVY_LOAD_END);
                previous_heavy = heavy;
            }
        }

        /* Poll input events */
        if (g_im) {
            InputEvent events[16];
            int n = input_monitor_poll(g_im, events, 16);
            for (int i = 0; i < n; i++) {
                if (g_sm) {
                    SceneState new_scene = state_machine_feed_event(g_sm, events[i].type);
                    log_debug("Input event %d → scene %d", events[i].type, new_scene);
                }
            }
        }

        /* Tick state machine (handles timeouts) */
        if (g_sm) {
            state_machine_tick(g_sm);
            SceneState current_scene = state_machine_get_scene(g_sm);
            scheduling_scene = current_scene;
            if (current_scene != published_scene) {
                if (g_dbus)
                    dbus_manager_set_scene(g_dbus,
                                           scene_to_string(current_scene));
                published_scene = current_scene;
            }
            ActionParams params;
            state_machine_get_actions(g_sm, &params);
            int requested_ms = (int)(params.base_sample_time * 1000.0f);
            if (requested_ms < 10) requested_ms = 10;
            if (requested_ms > 250) requested_ms = 250;
            poll_interval_ms = requested_ms;
        }

        apply_automatic_frequency_control(cpu_loads, nr_cpu_loads);
        free(cpu_loads);

        /* Periodic game scan (every 5 seconds) */
        static uint64_t last_scan = 0;
        uint64_t now = (uint64_t)time(NULL);
        if (now - last_scan > 5) {
            last_scan = now;
            if (g_scanner) {
                game_scanner_scan(g_scanner);
                /* Update DBus with game list + per-app modes */
                g_nr_game_results = game_scanner_get_results(
                    g_scanner, g_game_results, MAX_GAMES);
                int nr = g_nr_game_results;
                PowerMode detected_mode = MODE_BALANCE;
                for (int i = 0; i < nr; i++) {
                    const char *app_mode = game_scanner_get_app_mode(
                        g_scanner, g_game_results[i].comm);
                    if (strcmp(app_mode, "performance") == 0 ||
                        strcmp(app_mode, "fast") == 0) {
                        detected_mode = MODE_PERFORMANCE;
                        break;
                    }
                    if (strcmp(app_mode, "powersave") == 0)
                        detected_mode = MODE_POWERSAVE;
                }
                if (detected_mode != g_last_detected_app_mode) {
                    g_last_detected_app_mode = detected_mode;
                    apply_power_mode(
                        detected_mode == MODE_BALANCE
                            ? g_requested_mode : detected_mode,
                        detected_mode == MODE_BALANCE
                            ? "per-app override ended" : "per-app rule");
                }
                if (g_dbus) {
                    GameProcessEntry dbus_games[MAX_GAMES];
                    for (int i = 0; i < nr; i++) {
                        dbus_games[i].pid = g_game_results[i].pid;
                        dbus_games[i].comm = g_strdup(g_game_results[i].comm);
                        dbus_games[i].cmdline = g_strdup(
                            g_game_results[i].cmdline);
                        dbus_games[i].package = g_strdup(
                            g_game_results[i].package);
                        /* Look up per-app mode */
                        dbus_games[i].mode = g_strdup(
                            game_scanner_get_app_mode(
                                g_scanner, g_game_results[i].comm));
                    }
                    dbus_manager_update_games(g_dbus, dbus_games, nr);
                    for (int i = 0; i < nr; i++) {
                        g_free(dbus_games[i].comm);
                        g_free(dbus_games[i].cmdline);
                        g_free(dbus_games[i].package);
                        g_free(dbus_games[i].mode);
                    }
                }
            }
        }

        /* Apply process/thread scheduling at a bounded cadence. The task
         * scheduler itself performs slower full process discovery and faster
         * active-thread reconciliation. */
        gint64 policy_now_us = g_get_monotonic_time();
        if (last_policy_update_us == 0 ||
            policy_now_us - last_policy_update_us >= 250000) {
            last_policy_update_us = policy_now_us;
            if (g_task_scheduler &&
                task_scheduler_update(g_task_scheduler, g_game_results,
                                      g_nr_game_results,
                                      scheduling_scene) != 0 &&
                (last_sched_error_log_us == 0 ||
                 policy_now_us - last_sched_error_log_us >= 5000000)) {
                log_warn("Task scheduling reconciliation reported errors");
                last_sched_error_log_us = policy_now_us;
            }
            if (g_task_scheduler && g_dbus)
                dbus_manager_set_active_pid(
                    g_dbus, task_scheduler_get_active_pid(g_task_scheduler));
            if (g_task_scheduler) {
                TaskWorkload workloads[MAX_GAMES + MAX_RULES];
                int nr_workloads = task_scheduler_get_workloads(
                    g_task_scheduler, workloads,
                    (int)(sizeof(workloads) / sizeof(workloads[0])));

                if (nr_workloads >= 0 && g_cgroup_manager &&
                    cgroup_manager_update(g_cgroup_manager, workloads,
                                          nr_workloads) != 0 &&
                    (last_cgroup_error_log_us == 0 ||
                     policy_now_us - last_cgroup_error_log_us >= 5000000)) {
                    log_warn("Cgroup reconciliation reported errors");
                    last_cgroup_error_log_us = policy_now_us;
                }

                /* Surface scheduler status on D-Bus for the GUI. */
                if (g_dbus && nr_workloads >= 0) {
                    DbusWorkloadEntry entries[MAX_GAMES + MAX_RULES];
                    int nr_entries = 0;
                    for (int w = 0; w < nr_workloads &&
                         nr_entries < (int)(sizeof(entries) / sizeof(entries[0]));
                         w++) {
                        char comm[MAX_NAME_LEN];
                        if (!read_pid_comm(workloads[w].pid, comm, sizeof(comm)))
                            g_strlcpy(comm, "?", sizeof(comm));
                        entries[nr_entries].pid = (gint32)workloads[w].pid;
                        g_strlcpy(entries[nr_entries].comm, comm,
                                  sizeof(entries[nr_entries].comm));
                        g_strlcpy(entries[nr_entries].cgroup_class,
                                  workloads[w].cgroup_class[0]
                                      ? workloads[w].cgroup_class : "—",
                                  sizeof(entries[nr_entries].cgroup_class));
                        nr_entries++;
                    }
                    dbus_manager_update_scheduler(
                        g_dbus,
                        task_scheduler_tracked_processes(g_task_scheduler),
                        task_scheduler_tracked_threads(g_task_scheduler),
                        entries, nr_entries);
                }
            }
        }

        /* Sleep for poll interval */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)poll_interval_ms * 1000000L };
        nanosleep(&ts, NULL);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    const char *config_path = "/etc/uperf-linux/config.json";
    LogLevel log_level = UPERF_LOG_INFO;
    const char *log_file = NULL;
    int use_journald = 0;

    static struct option long_opts[] = {
        {"config",    required_argument, 0, 'c'},
        {"log-level", required_argument, 0, 'l'},
        {"log-file",  required_argument, 0, 'L'},
        {"journald",  no_argument,       0, 'j'},
        {"help",      no_argument,       0, 'h'},
        {"version",   no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:L:jhv", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'l': log_level = (LogLevel)atoi(optarg); break;
            case 'L': log_file = optarg; break;
            case 'j': use_journald = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            case 'v': print_version(); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }
    if (snprintf(g_config_path, sizeof(g_config_path), "%s", config_path) >=
        (int)sizeof(g_config_path)) {
        fprintf(stderr, "Config path is too long\n");
        return 1;
    }

    /* Initialize logging */
    if (log_init(log_level, use_journald, log_file) < 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return 1;
    }

    log_info("=== uperf-linux starting ===");
    print_version();

    /* Set up signal handlers */
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* Load configuration */
    log_info("Loading config: %s", g_config_path);
    if (config_load(&g_config, g_config_path) < 0) {
        log_fatal("Failed to load config from '%s'", g_config_path);
        return 1;
    }

    /* Validate config */
    if (config_validate(&g_config) < 0) {
        log_fatal("Config validation failed");
        return 1;
    }

    /* Check sysfs paths */
    config_check_paths(&g_config);

    /* Initialize components */
    g_sm = state_machine_new(&g_config);
    if (!g_sm) {
        log_fatal("Failed to create state machine");
        return 1;
    }

    g_writer = sysfs_writer_new(&g_config, 1000000);  /* 1ms batch window */
    if (!g_writer) {
        log_warn("Failed to create sysfs writer (continuing without batching)");
    } else {
        discover_manual_frequency_targets();
    }

    g_detector = heavyload_detector_new(
        10.0f,    /* 10ms sample time */
        60.0f,    /* heavy load threshold (%) */
        20.0f,    /* idle load threshold (%) */
        3000.0f   /* 3 second burst slack */
    );
    if (!g_detector) {
        log_warn("Failed to create heavy load detector");
    }

    g_im = create_input_monitor(&g_config);

    g_scanner = game_scanner_new();
    if (g_scanner) {
        /* Load per-app mode rules */
        game_scanner_perapp_scan(g_scanner, g_config.switcher.perapp_file);
        game_scanner_scan(g_scanner);
        g_nr_game_results = game_scanner_get_results(
            g_scanner, g_game_results, MAX_GAMES);
    }

    g_task_scheduler = task_scheduler_new(&g_config, SCHEDULER_STATE_FILE);
    g_cgroup_manager = cgroup_manager_new(&g_config, CGROUP_STATE_FILE);
    if (!g_task_scheduler || !g_cgroup_manager ||
        (g_config.cgroup.enable &&
         !cgroup_manager_is_available(g_cgroup_manager))) {
        log_fatal("Failed to initialize required sched/cgroup managers");
        cgroup_manager_free(g_cgroup_manager);
        task_scheduler_free(g_task_scheduler);
        game_scanner_free(g_scanner);
        input_monitor_free(g_im);
        heavyload_detector_free(g_detector);
        sysfs_writer_free(g_writer);
        state_machine_free(g_sm);
        config_free(&g_config);
        log_shutdown();
        return 1;
    }

    /* Initialize DBus manager */
    g_dbus = dbus_manager_new(G_BUS_TYPE_SYSTEM);
    if (g_dbus) {
        dbus_manager_set_mode_handler(g_dbus, dbus_mode_handler, NULL);
        dbus_manager_set_reload_handler(g_dbus, dbus_reload_handler, NULL);
        dbus_manager_set_game_mode_handler(g_dbus,
                                           dbus_game_mode_handler, NULL);
        dbus_manager_set_manual_freq_handler(g_dbus,
                                             dbus_manual_freq_handler, NULL);
        dbus_manager_set_active_pid_handler(g_dbus,
                                            dbus_active_pid_handler, NULL);
        log_info("DBus manager initialized on system bus");
    } else {
        log_warn("Failed to initialize DBus manager (GUI will not be able to control daemon)");
    }

    /* Initialize thermal manager from the selected device config. */
    g_thermal = create_thermal_manager(&g_config);

    /* Apply initial state */
    apply_initial_state();

    /* Run the main event loop */
    int ret = event_loop();

    /* Cleanup */
    log_info("Shutting down...");
    if (restore_frequency_targets()) {
        if (frequency_state_clear(FREQUENCY_STATE_FILE) != FREQUENCY_STATE_OK)
            log_warn("Could not remove frequency state snapshot");
    } else {
        log_error("One or more frequency limits could not be restored; "
                  "keeping recovery snapshot");
    }
    dbus_manager_free(g_dbus);
    thermal_manager_free(g_thermal);
    cgroup_manager_free(g_cgroup_manager);
    task_scheduler_free(g_task_scheduler);
    game_scanner_free(g_scanner);
    input_monitor_free(g_im);
    heavyload_detector_free(g_detector);
    sysfs_writer_free(g_writer);
    state_machine_free(g_sm);
    config_free(&g_config);
    log_shutdown();

    return ret;
}
