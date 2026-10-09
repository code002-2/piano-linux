#define _GNU_SOURCE
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/input.h>
#include <errno.h>
#include <stdint.h>

/* Bitops helpers for ioctl bitmasks */
#define ABSBIT_WORDS (((EV_ABS + 31) / 32) * ((ABS_MAX + 31) / 32))
static inline int test_bit(int nr, const unsigned long *addr) {
    return (addr[nr / (sizeof(unsigned long) * 8)] >> (nr % (sizeof(unsigned long) * 8))) & 1;
}

/* Simple JSON builder helpers — no external dependency */

/* Indentation level */
static int json_indent = 0;

static void json_indent_in(void)  { json_indent++; }
static void json_indent_out(void) { json_indent--; if (json_indent < 0) json_indent = 0; }

/* Read a file into a string buffer, return NULL on failure */
static char *read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    char *result = strdup(buf);
    if (result) {
        /* Strip trailing newlines */
        size_t len = strlen(result);
        while (len > 0 && (result[len-1] == '\n' || result[len-1] == '\r'))
            result[--len] = '\0';
    }
    return result;
}

/* Escape a string for JSON output */
static void json_write_string(FILE *fp, const char *s) {
    fputc('"', fp);
    while (*s) {
        switch (*s) {
            case '\\': fputs("\\\\", fp); break;
            case '"':  fputs("\\\"", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            default:   fputc(*s, fp); break;
        }
        s++;
    }
    fputc('"', fp);
}

/* ---- Hardware detection functions ---- */

typedef struct {
    int policy_id;
    int nr_cpus;
    uint64_t cpu_mask;
    long min_khz;
    long max_khz;
} CpuPolicy;

static int compare_policy_max_desc(const void *lhs, const void *rhs) {
    const CpuPolicy *a = lhs;
    const CpuPolicy *b = rhs;
    if (a->max_khz < b->max_khz) return 1;
    if (a->max_khz > b->max_khz) return -1;
    return a->policy_id - b->policy_id;
}

static long read_long_file(const char *path) {
    char *value = read_file(path);
    if (!value) return 0;
    char *end = NULL;
    errno = 0;
    long result = strtol(value, &end, 10);
    if (errno != 0 || end == value) result = 0;
    free(value);
    return result;
}

static uint64_t parse_cpu_list_mask(const char *list) {
    uint64_t mask = 0;
    const char *cursor = list;
    while (cursor && *cursor) {
        while (isspace((unsigned char)*cursor) || *cursor == ',') cursor++;
        if (!isdigit((unsigned char)*cursor)) break;
        char *end = NULL;
        long first = strtol(cursor, &end, 10);
        if (end == cursor || first < 0 || first >= 64) return 0;
        cursor = end;
        long last = first;
        if (*cursor == '-') {
            cursor++;
            last = strtol(cursor, &end, 10);
            if (end == cursor || last < first || last >= 64) return 0;
            cursor = end;
        }
        for (long cpu = first; cpu <= last; cpu++)
            mask |= UINT64_C(1) << cpu;
    }
    return mask;
}

static void emit_cpu_mask_array(FILE *fp, uint64_t mask) {
    fputc('[', fp);
    int written = 0;
    for (int cpu = 0; cpu < 64; cpu++) {
        if ((mask & (UINT64_C(1) << cpu)) == 0) continue;
        if (written++ > 0) fputs(", ", fp);
        fprintf(fp, "%d", cpu);
    }
    fputc(']', fp);
}

/* Detect CPU topology from /sys/devices/system/cpu/ */
static void detect_cpu_topology(FILE *fp) {
    fprintf(fp, "  \"cpu\": {\n");
    json_indent_in();

    fprintf(fp, "    \"enable\": true,\n");
    fprintf(fp, "    \"powerModel\": [\n");

    CpuPolicy policies[4] = {0};
    int nr_policies = 0;
    DIR *policy_dir = opendir("/sys/devices/system/cpu/cpufreq");
    if (policy_dir) {
        struct dirent *entry;
        while ((entry = readdir(policy_dir)) != NULL && nr_policies < 4) {
            if (strncmp(entry->d_name, "policy", 6) != 0 ||
                !isdigit((unsigned char)entry->d_name[6]))
                continue;

            char *end = NULL;
            long policy_id = strtol(entry->d_name + 6, &end, 10);
            if (!end || *end != '\0' || policy_id < 0) continue;

            char path[512];
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpufreq/%s/related_cpus",
                     entry->d_name);
            char *related = read_file(path);
            uint64_t cpu_mask = parse_cpu_list_mask(related);
            int nr_cpus = __builtin_popcountll(cpu_mask);
            free(related);
            if (nr_cpus <= 0) continue;

            CpuPolicy *policy = &policies[nr_policies];
            policy->policy_id = (int)policy_id;
            policy->nr_cpus = nr_cpus;
            policy->cpu_mask = cpu_mask;
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpufreq/%s/cpuinfo_min_freq",
                     entry->d_name);
            policy->min_khz = read_long_file(path);
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpufreq/%s/cpuinfo_max_freq",
                     entry->d_name);
            policy->max_khz = read_long_file(path);
            if (policy->max_khz <= 0) {
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/cpufreq/%s/scaling_max_freq",
                         entry->d_name);
                policy->max_khz = read_long_file(path);
            }
            if (policy->max_khz <= 0) continue;
            nr_policies++;
        }
        closedir(policy_dir);
    }

    if (nr_policies == 0) {
        long online = sysconf(_SC_NPROCESSORS_ONLN);
        policies[0] = (CpuPolicy){
            .policy_id = 0,
            .nr_cpus = online > 0 ? (int)online : 1,
            .min_khz = 300000,
            .max_khz = 1800000,
        };
        int fallback_cpus = policies[0].nr_cpus < 64
            ? policies[0].nr_cpus : 64;
        policies[0].cpu_mask = fallback_cpus == 64
            ? UINT64_MAX : (UINT64_C(1) << fallback_cpus) - 1;
        nr_policies = 1;
    }

    qsort(policies, (size_t)nr_policies, sizeof(policies[0]),
          compare_policy_max_desc);

    for (int c = 0; c < nr_policies; c++) {
        float max_mhz = policies[c].max_khz / 1000.0f;
        float min_mhz = policies[c].min_khz > 0
            ? policies[c].min_khz / 1000.0f : max_mhz * 0.2f;
        float free_mhz = max_mhz * 0.25f;
        if (free_mhz < min_mhz) free_mhz = min_mhz;
        int efficiency = nr_policies == 1
            ? 250 : 180 + (nr_policies - 1 - c) * 85;
        float typical_power = 0.8f + (nr_policies - 1 - c) * 0.3f;

        if (c > 0) fprintf(fp, ",\n");
        fprintf(fp, "      {\n");
        fprintf(fp, "        \"efficiency\": %d,\n", efficiency);
        fprintf(fp, "        \"nr\": %d,\n", policies[c].nr_cpus);
        fprintf(fp, "        \"cpus\": ");
        emit_cpu_mask_array(fp, policies[c].cpu_mask);
        fprintf(fp, ",\n");
        fprintf(fp, "        \"typicalPower\": %.1f,\n", typical_power);
        fprintf(fp, "        \"typicalFreq\": %.0f,\n", max_mhz);
        fprintf(fp, "        \"sweetFreq\": %.0f,\n", max_mhz * 0.75f);
        fprintf(fp, "        \"plainFreq\": %.0f,\n", max_mhz * 0.55f);
        fprintf(fp, "        \"freeFreq\": %.0f\n", free_mhz);
        fprintf(fp, "      }");
    }
    fprintf(fp, "\n    ]\n");

    json_indent_out();
    fprintf(fp, "  },\n");
}

static void emit_sysfs_knob(FILE *fp, int *count, const char *name,
                            const char *path) {
    if ((*count)++ > 0) fprintf(fp, ",\n");
    fprintf(fp, "      ");
    json_write_string(fp, name);
    fprintf(fp, ": ");
    json_write_string(fp, path);
}

/* Detect sysfs knobs from /sys/class/devfreq/ */
static void detect_devfreq(FILE *fp) {
    fprintf(fp, "  \"sysfs\": {\n");
    json_indent_in();
    fprintf(fp, "    \"enable\": true,\n");
    fprintf(fp, "    \"knob\": {\n");

    int nr_knobs = 0;
    emit_sysfs_knob(fp, &nr_knobs, "cpufreqMax",
                    "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq");
    emit_sysfs_knob(fp, &nr_knobs, "cpufreqMin",
                    "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq");
    emit_sysfs_knob(fp, &nr_knobs, "cpufreqGovernor",
                    "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor");

    /* Scan devfreq devices */
    DIR *df = opendir("/sys/class/devfreq");
    if (df) {
        struct dirent *ent;
        while ((ent = readdir(df)) != NULL) {
            if (ent->d_name[0] == '.') continue;

            char name_path[512];
            snprintf(name_path, sizeof(name_path), "/sys/class/devfreq/%s/name",
                     ent->d_name);
            char *device_name = read_file(name_path);
            const char *name = device_name ? device_name : ent->d_name;

            const char *prefix = NULL;
            if (strstr(name, "gpu")) prefix = "gpu";
            else if (strstr(name, "cpu-cpu-llcc-bw")) prefix = "memBw";
            else if (strstr(name, "cpu-llcc-ddr-bw")) prefix = "ddr";
            else if (strstr(name, "soc")) prefix = "soc";

            if (prefix) {
                char knob_name[64];
                char path[512];
                snprintf(path, sizeof(path), "/sys/class/devfreq/%s/max_freq",
                         ent->d_name);
                if (access(path, F_OK) == 0) {
                    snprintf(knob_name, sizeof(knob_name), "%sMaxFreq", prefix);
                    emit_sysfs_knob(fp, &nr_knobs, knob_name, path);
                }
                snprintf(path, sizeof(path), "/sys/class/devfreq/%s/min_freq",
                         ent->d_name);
                if (access(path, F_OK) == 0) {
                    snprintf(knob_name, sizeof(knob_name), "%sMinFreq", prefix);
                    emit_sysfs_knob(fp, &nr_knobs, knob_name, path);
                }
                snprintf(path, sizeof(path), "/sys/class/devfreq/%s/governor",
                         ent->d_name);
                if (access(path, F_OK) == 0) {
                    snprintf(knob_name, sizeof(knob_name), "%sGovernor", prefix);
                    emit_sysfs_knob(fp, &nr_knobs, knob_name, path);
                }
            }
            free(device_name);
        }
        closedir(df);
    }

    fprintf(fp, "\n");

    json_indent_out();
    fprintf(fp, "    }\n");
    json_indent_out();
    fprintf(fp, "  },\n");
}

/* Detect thermal zones */
static void detect_thermal(FILE *fp) {
    fprintf(fp, "  \"thermal\": {\n");
    json_indent_in();
    fprintf(fp, "    \"enabled\": true,\n");
    fprintf(fp, "    \"warn_temp\": 70000,\n");
    fprintf(fp, "    \"throttle_temp\": 80000,\n");
    fprintf(fp, "    \"critical_temp\": 95000,\n");
    fprintf(fp, "    \"recovery_temp\": 75000,\n");

    fprintf(fp, "    \"monitor_all_zones\": true\n");

    json_indent_out();
    fprintf(fp, "  },\n");
}

/* Detect touchscreen input devices */
static void detect_touchscreen(FILE *fp) {
    fprintf(fp, "  \"input\": {\n");
    json_indent_in();
    /* enable is written after probing /dev/input below */

    /* Check for touchscreen devices by looking for ABS_MT axes */
    int has_touch = 0;
    DIR *input_dir = opendir("/dev/input");
    if (input_dir) {
        struct dirent *ent;
        /* Scan for event devices and check for MT capability */
        while ((ent = readdir(input_dir)) != NULL && !has_touch) {
            if (strncmp(ent->d_name, "event", 5) != 0) continue;
            char evdev_path[512];
            snprintf(evdev_path, sizeof(evdev_path), "/dev/input/%s", ent->d_name);
            int fd = open(evdev_path, O_RDONLY);
            if (fd >= 0) {
                /* Check for ABS_MT_POSITION_X (0x35) — definitive touchscreen indicator */
                unsigned long absbit[ABSBIT_WORDS]; memset(absbit, 0, sizeof(absbit));
                if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) == 0) {
                    if (test_bit(ABS_MT_POSITION_X, absbit) ||
                        test_bit(ABS_MT_POSITION_Y, absbit)) {
                        has_touch = 1;
                    }
                }
                close(fd);
            }
        }
        closedir(input_dir);
    }

    /* Unprivileged users often cannot open /dev/input/event*. Fall back to
     * the world-readable sysfs device names so detect does not disable touch
     * merely because it was run without membership in the input group. */
    if (!has_touch) {
        DIR *sys_input = opendir("/sys/class/input");
        if (sys_input) {
            struct dirent *ent;
            while ((ent = readdir(sys_input)) != NULL && !has_touch) {
                if (strncmp(ent->d_name, "event", 5) != 0) continue;
                char name_path[512];
                snprintf(name_path, sizeof(name_path),
                         "/sys/class/input/%s/device/name", ent->d_name);
                char *name = read_file(name_path);
                if (name && strcasestr(name, "touchscreen")) has_touch = 1;
                free(name);
            }
            closedir(sys_input);
        }
    }

    /* Try to read screen resolution from DRM/KMS or fbdev */
    int screen_w = 1080, screen_h = 2400;  /* Default for tablet */
    char drm_path[512];
    DIR *drm_dir = opendir("/sys/class/drm");
    if (drm_dir) {
        struct dirent *ent;
        while ((ent = readdir(drm_dir)) != NULL && (screen_w == 1080 || screen_h == 2400)) {
            /* Look for active connector: .../card0-<name>/modes */
            if (strncmp(ent->d_name, "card0-", 6) != 0) continue;
            snprintf(drm_path, sizeof(drm_path),
                     "/sys/class/drm/%s/modes", ent->d_name);
            char *modes = read_file(drm_path);
            if (modes) {
                /* Format: WxH (e.g., "1080x2400") */
                int w = 0, h = 0;
                if (sscanf(modes, "%dx%d", &w, &h) == 2) {
                    screen_w = w;
                    screen_h = h;
                }
                free(modes);
            }
        }
        closedir(drm_dir);
    }

    fprintf(fp, "    \"enable\": %s,\n", has_touch ? "true" : "false");
    fprintf(fp, "    \"screen_width\": %d,\n", screen_w);
    fprintf(fp, "    \"screen_height\": %d,\n", screen_h);
    fprintf(fp, "    \"swipeThd\": 0.03,\n");
    fprintf(fp, "    \"gestureThdX\": 0.03,\n");
    fprintf(fp, "    \"gestureThdY\": 0.03,\n");
    fprintf(fp, "    \"gestureDelayTime\": 2.0,\n");
    fprintf(fp, "    \"holdEnterTime\": 1.0\n");
    json_indent_out();
    fprintf(fp, "  }\n");
}

/* Generate a baseline JSON config */
static void generate_config(FILE *fp, const char *soc_name) {
    fprintf(fp, "{\n");
    fprintf(fp, "  \"meta\": {\n");
    fprintf(fp, "    \"name\": \"%s [auto-generated]\",\n", soc_name);
    fprintf(fp, "    \"author\": \"uperf-linux config wizard\"\n");
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"modules\": {\n");
    json_indent_in();

    /* Switcher */
    fprintf(fp, "    \"switcher\": {\n");
    json_indent_in();
    fprintf(fp, "      \"switchInode\": \"/run/uperf-linux/cur_powermode\",\n");
    fprintf(fp, "      \"perapp\": \"/etc/uperf-linux/perapp_powermode\",\n");
    fprintf(fp, "      \"hintDuration\": {\n");
    json_indent_in();
    fprintf(fp, "\"idle\": 0.0,\n");
    fprintf(fp, "\"touch\": 4.0,\n");
    fprintf(fp, "\"trigger\": 0.03,\n");
    fprintf(fp, "\"gesture\": 0.1,\n");
    fprintf(fp, "\"junk\": 0.06,\n");
    fprintf(fp, "\"switch\": 0.4,\n");
    fprintf(fp, "\"boost\": 0.0\n");
    json_indent_out();
    fprintf(fp, "      }\n");
    json_indent_out();
    fprintf(fp, "    },\n");

    detect_cpu_topology(fp);
    detect_devfreq(fp);
    detect_thermal(fp);
    detect_touchscreen(fp);

    json_indent_out();
    fprintf(fp, "  },\n");

    /* Initials */
    fprintf(fp, "  \"initials\": {\n");
    json_indent_in();
    fprintf(fp, "    \"cpu\": {\n");
    json_indent_in();
    fprintf(fp, "\"baseSampleTime\": 0.01,\n");
    fprintf(fp, "\"baseSlackTime\": 0.01,\n");
    fprintf(fp, "\"latencyTime\": 0.2,\n");
    fprintf(fp, "\"slowLimitPower\": 3.0,\n");
    fprintf(fp, "\"fastLimitPower\": 6.0,\n");
    fprintf(fp, "\"fastLimitCapacity\": 10.0,\n");
    fprintf(fp, "\"fastLimitRecoverScale\": 0.3,\n");
    fprintf(fp, "\"predictThd\": 0.3,\n");
    fprintf(fp, "\"margin\": 0.25,\n");
    fprintf(fp, "\"burst\": 0.0,\n");
    fprintf(fp, "\"guideCap\": false,\n");
    fprintf(fp, "\"limitEfficiency\": false\n");
    json_indent_out();
    fprintf(fp, "    }\n");
    json_indent_out();
    fprintf(fp, "  },\n");

    /* Presets */
    fprintf(fp, "  \"presets\": {\n");
    fprintf(fp, "    \"balance\": {\n");
    fprintf(fp, "      \"*\": { \"cpu.margin\": 0.2 },\n");
    fprintf(fp, "      \"idle\": { \"cpu.baseSampleTime\": 0.04 },\n");
    fprintf(fp, "      \"touch\": { \"cpu.margin\": 0.4 },\n");
    fprintf(fp, "      \"switch\": { \"cpu.latencyTime\": 0.0, \"cpu.margin\": 0.4 }\n");
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"powersave\": {\n");
    fprintf(fp, "      \"*\": { \"cpu.latencyTime\": 0.4, \"cpu.margin\": 0.1 },\n");
    fprintf(fp, "      \"idle\": { \"cpu.baseSampleTime\": 0.04, \"cpu.limitEfficiency\": true }\n");
    fprintf(fp, "    },\n");
    fprintf(fp, "    \"performance\": {\n");
    fprintf(fp, "      \"*\": { \"cpu.latencyTime\": 0.0, \"cpu.margin\": 0.4, \"cpu.burst\": 0.2 }\n");
    fprintf(fp, "    }\n");
    fprintf(fp, "  }\n");

    fprintf(fp, "}\n");
}

/* ---- CLI ---- */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <command> [args]\n"
        "\n"
        "Commands:\n"
        "  detect              Scan hardware and print baseline JSON config to stdout\n"
        "  detect --output F   Write baseline config to file F\n"
        "  help                Show this help\n",
        prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *output_file = NULL;
    const char *soc_name = "detected-soc";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "help") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "version") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("uperf-linux config wizard v0.1.0\n");
            return 0;
        }
        if (strcmp(argv[i], "detect") == 0) {
            /* Next arg is --output or soc name */
            if (i + 1 < argc) {
                if (strcmp(argv[i+1], "--output") == 0 && i + 2 < argc) {
                    output_file = argv[i+2];
                    i += 2;
                } else {
                    soc_name = argv[i+1];
                    i++;
                }
            }
        }
    }

    FILE *fp = stdout;
    int close_fp = 0;

    if (output_file) {
        fp = fopen(output_file, "w");
        if (!fp) {
            fprintf(stderr, "Error: cannot open '%s': %s\n", output_file, strerror(errno));
            return 1;
        }
        close_fp = 1;
    }

    generate_config(fp, soc_name);

    if (close_fp) {
        fclose(fp);
        fprintf(stderr, "Config written to %s\n", output_file);
    } else {
        fprintf(stderr,
                "(config printed to stdout — redirect with > config.json)\n");
    }

    return 0;
}
