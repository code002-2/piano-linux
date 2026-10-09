#define _GNU_SOURCE

#include "cgroup_manager.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#endif

#define SYSTEMD_DEST "org.freedesktop.systemd1"
#define SYSTEMD_PATH "/org/freedesktop/systemd1"
#define SYSTEMD_MANAGER "org.freedesktop.systemd1.Manager"
#define MAX_MANAGED_UNITS (MAX_GAMES + MAX_RULES)
#define MAX_UNIT_NAME 256
#define MAX_UNIT_PIDS 1024
#define MAX_UNIT_ROOTS 32
#define PROPERTY_VERIFY_INTERVAL_MS 5000

/*
 * systemd remains the sole cgroup writer.  The manager changes properties of
 * the workload's existing unit and never moves processes between cgroups.
 * Thread-level affinity and utilization clamps are handled by TaskScheduler.
 */
typedef struct {
    uid_t uid;
    bool user_manager;
    uint64_t seen_generation;
    char class_name[MAX_NAME_LEN];
    char unit[MAX_UNIT_NAME];
    uint64_t original_weight;
    uint64_t original_allowed_cpus;
    uint64_t last_verified_ms;
} ManagedUnit;

typedef struct {
    uid_t uid;
    bool user_manager;
    int score;
    const CgroupClass *klass;
    pid_t roots[MAX_UNIT_ROOTS];
    int nr_roots;
    bool roots_complete;
    char unit[MAX_UNIT_NAME];
} DesiredUnit;

struct CgroupManager {
    bool enabled;
    bool available;
    CgroupConfig config;
    ManagedUnit units[MAX_MANAGED_UNITS];
    int nr_units;
    uint64_t generation;
    bool state_dirty;
    char state_path[MAX_PATH_LEN];
};

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u +
        (uint64_t)now.tv_nsec / 1000000u;
}

static int read_text_file(const char *path, char *buffer, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t count = read(fd, buffer, size - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (count < 0) return -1;
    buffer[count] = '\0';
    return (int)count;
}

static bool read_process_stat(pid_t pid, pid_t *ppid, uint64_t *start_time) {
    char path[64];
    char buffer[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_text_file(path, buffer, sizeof(buffer)) < 0) return false;
    char *cursor = strrchr(buffer, ')');
    if (!cursor || cursor[1] != ' ') return false;
    cursor += 2;
    if (cursor[0] == '\0' || cursor[1] != ' ') return false;
    cursor += 2;
    for (int field = 4; field <= 22; field++) {
        while (*cursor == ' ') cursor++;
        if (*cursor == '\0') return false;
        char *end = NULL;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (end == cursor) return false;
        if (field == 4 && ppid) *ppid = (pid_t)value;
        if (field == 22 && start_time) *start_time = (uint64_t)value;
        cursor = end;
    }
    return true;
}

static bool process_matches(pid_t pid, uint64_t start_time) {
    uint64_t current = 0;
    return read_process_stat(pid, NULL, &current) && current == start_time;
}

static bool is_descendant_or_same(pid_t pid, pid_t root) {
    if (pid == root) return true;
    pid_t current = pid;
    for (int depth = 0; depth < 128 && current > 1; depth++) {
        pid_t parent = 0;
        if (!read_process_stat(current, &parent, NULL) || parent <= 0 ||
            parent == current)
            return false;
        if (parent == root) return true;
        current = parent;
    }
    return false;
}

static const CgroupClass *find_class(const CgroupManager *manager,
                                     const char *name) {
    if (!manager || !name) return NULL;
    for (int i = 0; i < manager->config.nr_classes; i++) {
        if (strcmp(manager->config.classes[i].name, name) == 0)
            return &manager->config.classes[i];
    }
    return NULL;
}

#ifdef HAVE_LIBSYSTEMD

static sd_bus *open_user_manager(uid_t uid) {
    char address[128];
    snprintf(address, sizeof(address),
             "unix:path=/run/user/%u/systemd/private", (unsigned int)uid);
    sd_bus *bus = NULL;
    int result = sd_bus_new(&bus);
    if (result < 0) {
        log_warn("cgroup: sd_bus_new for user %u failed: %s",
                 (unsigned int)uid, strerror(-result));
        return NULL;
    }
    /* systemd/private is a direct D-Bus peer, not a bus broker. */
    result = sd_bus_set_address(bus, address);
    if (result >= 0) result = sd_bus_start(bus);
    if (result < 0) {
        log_warn("cgroup: cannot connect to user %u manager: %s",
                 (unsigned int)uid, strerror(-result));
        sd_bus_unref(bus);
        return NULL;
    }
    return bus;
}

static sd_bus *open_manager_bus(bool user_manager, uid_t uid) {
    sd_bus *bus = NULL;
    if (user_manager) return open_user_manager(uid);
    if (sd_bus_open_system(&bus) < 0) return NULL;
    return bus;
}

static int get_unit_for_pid(pid_t pid, bool *user_manager, char *unit,
                            size_t unit_size) {
    char *name = NULL;
    int result = sd_pid_get_user_unit(pid, &name);
    if (result >= 0 && name) {
        *user_manager = true;
    } else {
        free(name);
        name = NULL;
        result = sd_pid_get_unit(pid, &name);
        *user_manager = false;
    }
    if (result < 0 || !name) return -1;
    int length = snprintf(unit, unit_size, "%s", name);
    free(name);
    return length >= 0 && (size_t)length < unit_size ? 0 : -1;
}

static const char *unit_interface(const char *unit) {
    size_t length = strlen(unit);
    if (length >= 6 && strcmp(unit + length - 6, ".scope") == 0)
        return "org.freedesktop.systemd1.Scope";
    if (length >= 8 && strcmp(unit + length - 8, ".service") == 0)
        return "org.freedesktop.systemd1.Service";
    if (length >= 6 && strcmp(unit + length - 6, ".slice") == 0)
        return "org.freedesktop.systemd1.Slice";
    return "org.freedesktop.systemd1.Unit";
}

static int get_unit_path(sd_bus *bus, const char *unit, char **path) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *borrowed_path = NULL;
    int result = sd_bus_call_method(bus, SYSTEMD_DEST, SYSTEMD_PATH,
                                    SYSTEMD_MANAGER, "GetUnit", &error,
                                    &reply, "s", unit);
    if (result >= 0)
        result = sd_bus_message_read(reply, "o", &borrowed_path);
    if (result >= 0) {
        *path = strdup(borrowed_path);
        if (!*path) result = -ENOMEM;
    }
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return result;
}

static int read_unit_properties(sd_bus *bus, const char *unit,
                                uint64_t *weight, uint64_t *allowed_mask) {
    char *path = NULL;
    int result = get_unit_path(bus, unit, &path);
    if (result < 0) return result;
    const char *interface = unit_interface(unit);
    uint64_t current_weight = UINT64_MAX;
    result = sd_bus_get_property_trivial(bus, SYSTEMD_DEST, path, interface,
                                         "CPUWeight", NULL, 't',
                                         &current_weight);
    if (result < 0) goto done;

    sd_bus_message *reply = NULL;
    result = sd_bus_get_property(bus, SYSTEMD_DEST, path, interface,
                                 "AllowedCPUs", NULL, &reply, "ay");
    if (result >= 0) {
        const void *bytes = NULL;
        size_t length = 0;
        result = sd_bus_message_read_array(reply, 'y', &bytes, &length);
        uint64_t mask = 0;
        if (result >= 0 && bytes) {
            const uint8_t *array = bytes;
            size_t limit = length < sizeof(mask) ? length : sizeof(mask);
            for (size_t i = 0; i < limit; i++)
                mask |= (uint64_t)array[i] << (i * 8);
        }
        if (result >= 0) {
            *weight = current_weight;
            *allowed_mask = mask;
        }
    }
    sd_bus_message_unref(reply);
done:
    free(path);
    return result;
}

static int append_property_u64(sd_bus_message *message, const char *name,
                               uint64_t value) {
    int result = sd_bus_message_open_container(message, 'r', "sv");
    if (result >= 0) result = sd_bus_message_append(message, "s", name);
    if (result >= 0)
        result = sd_bus_message_open_container(message, 'v', "t");
    if (result >= 0) result = sd_bus_message_append(message, "t", value);
    if (result >= 0) result = sd_bus_message_close_container(message);
    if (result >= 0) result = sd_bus_message_close_container(message);
    return result;
}

static int append_property_cpu_mask(sd_bus_message *message, const char *name,
                                    uint64_t mask) {
    uint8_t bytes[sizeof(mask)];
    size_t length = 0;
    for (uint64_t value = mask; value != 0; value >>= 8)
        bytes[length++] = (uint8_t)(value & 0xffu);
    int result = sd_bus_message_open_container(message, 'r', "sv");
    if (result >= 0) result = sd_bus_message_append(message, "s", name);
    if (result >= 0)
        result = sd_bus_message_open_container(message, 'v', "ay");
    if (result >= 0)
        result = sd_bus_message_append_array(message, 'y', bytes, length);
    if (result >= 0) result = sd_bus_message_close_container(message);
    if (result >= 0) result = sd_bus_message_close_container(message);
    return result;
}

static int set_unit_properties(sd_bus *bus, const char *unit, uint64_t weight,
                               uint64_t allowed_mask, bool set_allowed) {
    sd_bus_message *message = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int result = sd_bus_message_new_method_call(
        bus, &message, SYSTEMD_DEST, SYSTEMD_PATH, SYSTEMD_MANAGER,
        "SetUnitProperties");
    if (result >= 0) result = sd_bus_message_append(message, "sb", unit, 1);
    if (result >= 0)
        result = sd_bus_message_open_container(message, 'a', "(sv)");
    if (result >= 0) result = append_property_u64(message, "CPUWeight", weight);
    if (result >= 0 && set_allowed)
        result = append_property_cpu_mask(message, "AllowedCPUs", allowed_mask);
    if (result >= 0) result = sd_bus_message_close_container(message);
    if (result >= 0)
        result = sd_bus_call(bus, message, 0, &error, &reply);
    if (result < 0)
        log_warn("cgroup: SetUnitProperties(%s) failed: %s", unit,
                 error.message ? error.message : strerror(-result));
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(message);
    return result;
}

static bool unit_properties_match(uint64_t current_weight,
                                  uint64_t current_allowed,
                                  const CgroupClass *klass) {
    return current_weight == (uint64_t)klass->cpu_weight &&
        (klass->cpu_mask == 0 || current_allowed == klass->cpu_mask);
}

static int get_unit_processes(sd_bus *bus, const char *unit, pid_t *pids,
                              int capacity) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int result = sd_bus_call_method(bus, SYSTEMD_DEST, SYSTEMD_PATH,
                                    SYSTEMD_MANAGER, "GetUnitProcesses",
                                    &error, &reply, "s", unit);
    if (result < 0) goto done;
    result = sd_bus_message_enter_container(reply, 'a', "(sus)");
    if (result < 0) goto done;
    int count = 0;
    bool overflow = false;
    while ((result = sd_bus_message_enter_container(reply, 'r', "sus")) > 0) {
        const char *subgroup = NULL;
        const char *command = NULL;
        uint32_t pid = 0;
        result = sd_bus_message_read(reply, "sus", &subgroup, &pid, &command);
        if (result < 0) break;
        if (count < capacity)
            pids[count++] = (pid_t)pid;
        else
            overflow = true;
        result = sd_bus_message_exit_container(reply);
        if (result < 0) break;
    }
    if (result >= 0) result = overflow ? -E2BIG : count;
done:
    if (result < 0)
        log_warn("cgroup: GetUnitProcesses(%s) failed: %s", unit,
                 error.message ? error.message : strerror(-result));
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return result;
}

/* A unit is safe to tune when every process in it belongs to at least one of
 * the matched workload roots. Shared units such as dbus.service are left to
 * per-thread scheduling instead. */
static int unit_is_dedicated(sd_bus *bus, const DesiredUnit *unit) {
    pid_t pids[MAX_UNIT_PIDS];
    int count = get_unit_processes(bus, unit->unit, pids, MAX_UNIT_PIDS);
    if (count < 0) return -1;
    if (count == 0) return 0;
    for (int i = 0; i < count; i++) {
        bool matched = false;
        for (int j = 0; j < unit->nr_roots; j++) {
            if (is_descendant_or_same(pids[i], unit->roots[j])) {
                matched = true;
                break;
            }
        }
        if (!matched) return 0;
    }
    return 1;
}

static int restore_unit(const ManagedUnit *unit) {
    sd_bus *bus = open_manager_bus(unit->user_manager, unit->uid);
    if (!bus) return -1;
    char *path = NULL;
    int result = get_unit_path(bus, unit->unit, &path);
    free(path);
    if (result == -ENOENT) {
        sd_bus_unref(bus);
        return 0;
    }
    if (result >= 0)
        result = set_unit_properties(bus, unit->unit, unit->original_weight,
                                     unit->original_allowed_cpus, true);
    sd_bus_unref(bus);
    return result < 0 ? -1 : 0;
}

#else

static int restore_unit(const ManagedUnit *unit) {
    (void)unit;
    return -1;
}

#endif /* HAVE_LIBSYSTEMD */

static int find_managed(const CgroupManager *manager, uid_t uid,
                        bool user_manager, const char *unit) {
    for (int i = 0; i < manager->nr_units; i++) {
        const ManagedUnit *item = &manager->units[i];
        if (item->uid == uid && item->user_manager == user_manager &&
            strcmp(item->unit, unit) == 0)
            return i;
    }
    return -1;
}

static int persist_state(const CgroupManager *manager) {
    if (manager->state_path[0] == '\0') return 0;
    if (manager->nr_units == 0) {
        if (unlink(manager->state_path) != 0 && errno != ENOENT) return -1;
        return 0;
    }
    char temp[MAX_PATH_LEN + 32];
    int length = snprintf(temp, sizeof(temp), "%s.tmp.%d",
                          manager->state_path, getpid());
    if (length <= 0 || length >= (int)sizeof(temp)) return -1;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(temp);
        return -1;
    }
    fprintf(file, "UPERF_CGROUP_STATE 2\n");
    for (int i = 0; i < manager->nr_units; i++) {
        const ManagedUnit *item = &manager->units[i];
        fprintf(file, "U %u %d %llu %016llx %s\n",
                (unsigned int)item->uid, item->user_manager,
                (unsigned long long)item->original_weight,
                (unsigned long long)item->original_allowed_cpus, item->unit);
    }
    bool ok = fflush(file) == 0 && fsync(fd) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temp, manager->state_path) != 0) {
        unlink(temp);
        return -1;
    }
    return 0;
}

static void recover_state(const char *path) {
    if (!path || path[0] == '\0') return;
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[1024];
    if (!fgets(line, sizeof(line), file) ||
        strcmp(line, "UPERF_CGROUP_STATE 2\n") != 0) {
        fclose(file);
        log_warn("cgroup: ignoring incompatible recovery state %s", path);
        return;
    }
    int restored = 0;
    int failures = 0;
    while (fgets(line, sizeof(line), file)) {
        ManagedUnit item;
        memset(&item, 0, sizeof(item));
        unsigned int uid = 0;
        unsigned long long weight = 0, allowed = 0;
        int user_manager = 0;
        if (sscanf(line, "U %u %d %llu %llx %255s", &uid, &user_manager,
                   &weight, &allowed, item.unit) != 5)
            continue;
        item.uid = (uid_t)uid;
        item.user_manager = user_manager != 0;
        item.original_weight = (uint64_t)weight;
        item.original_allowed_cpus = (uint64_t)allowed;
        if (restore_unit(&item) == 0)
            restored++;
        else
            failures++;
    }
    fclose(file);
    if (failures == 0) unlink(path);
    log_info("cgroup: crash recovery restored %d unit(s), %d failure(s)",
             restored, failures);
}

CgroupManager *cgroup_manager_new(const Config *config,
                                  const char *state_path) {
    if (!config) return NULL;
    CgroupManager *manager = calloc(1, sizeof(*manager));
    if (!manager) return NULL;
    manager->config = config->cgroup;
    manager->enabled = config->cgroup.enable;
    if (state_path)
        snprintf(manager->state_path, sizeof(manager->state_path), "%s",
                 state_path);
    recover_state(manager->state_path);
#ifdef HAVE_LIBSYSTEMD
    manager->available = manager->enabled &&
        strcmp(manager->config.backend, "systemd") == 0;
#else
    manager->available = false;
#endif
    if (manager->enabled && !manager->available)
        log_error("cgroup: systemd backend requested but libsystemd is unavailable");
    else
        log_info("CgroupManager created: enabled=%d backend=%s",
                 manager->enabled, manager->config.backend);
    return manager;
}

void cgroup_manager_free(CgroupManager *manager) {
    if (!manager) return;
    for (int i = manager->nr_units - 1; i >= 0; i--) {
        if (restore_unit(&manager->units[i]) != 0) {
            log_warn("cgroup: failed to restore unit %s during shutdown",
                     manager->units[i].unit);
            continue;
        }
        manager->units[i] = manager->units[manager->nr_units - 1];
        manager->nr_units--;
    }
    if (persist_state(manager) != 0)
        log_warn("cgroup: failed to persist shutdown recovery state");
    free(manager);
}

int cgroup_manager_update(CgroupManager *manager,
                          const TaskWorkload *workloads, int nr_workloads) {
    if (!manager || nr_workloads < 0) return -1;
    if (!manager->enabled) return 0;
    if (!manager->available) return -1;
#ifndef HAVE_LIBSYSTEMD
    (void)workloads;
    return -1;
#else
    DesiredUnit desired[MAX_MANAGED_UNITS];
    int nr_desired = 0;
    int failures = 0;
    bool discovery_incomplete = false;
    uint64_t generation = ++manager->generation;
    uint64_t now = monotonic_ms();

    /* Resolve process rules to existing systemd units and coalesce rules that
     * share one unit.  Active/game rules win over background siblings. */
    for (int i = 0; workloads && i < nr_workloads; i++) {
        const TaskWorkload *input = &workloads[i];
        if (input->cgroup_class[0] == '\0' ||
            !process_matches(input->pid, input->start_time))
            continue;
        const CgroupClass *klass = find_class(manager, input->cgroup_class);
        if (!klass) {
            failures++;
            continue;
        }
        bool user_manager = false;
        char unit[MAX_UNIT_NAME];
        if (get_unit_for_pid(input->pid, &user_manager, unit,
                             sizeof(unit)) != 0) {
            log_warn("cgroup: no systemd unit found for PID %d", input->pid);
            failures++;
            discovery_incomplete = true;
            continue;
        }
        int score = (input->active ? 30000 : 0) +
                    (input->game ? 15000 : 0) + klass->cpu_weight;
        int index = -1;
        for (int j = 0; j < nr_desired; j++) {
            if (desired[j].uid == input->uid &&
                desired[j].user_manager == user_manager &&
                strcmp(desired[j].unit, unit) == 0) {
                index = j;
                break;
            }
        }
        if (index >= 0) {
            if (score > desired[index].score) {
                desired[index].score = score;
                desired[index].klass = klass;
            }
            if (desired[index].nr_roots < MAX_UNIT_ROOTS)
                desired[index].roots[desired[index].nr_roots++] = input->pid;
            else
                desired[index].roots_complete = false;
            continue;
        }
        if (nr_desired >= MAX_MANAGED_UNITS) {
            failures++;
            discovery_incomplete = true;
            continue;
        }
        DesiredUnit *item = &desired[nr_desired++];
        memset(item, 0, sizeof(*item));
        item->uid = input->uid;
        item->user_manager = user_manager;
        item->score = score;
        item->klass = klass;
        item->roots_complete = true;
        item->roots[item->nr_roots++] = input->pid;
        snprintf(item->unit, sizeof(item->unit), "%s", unit);
    }

    for (int i = nr_desired - 1; i >= 0; i--) {
        sd_bus *bus = open_manager_bus(desired[i].user_manager,
                                       desired[i].uid);
        int dedicated = bus && desired[i].roots_complete
            ? unit_is_dedicated(bus, &desired[i]) : -1;
        sd_bus_unref(bus);
        if (dedicated < 0) {
            int index = find_managed(manager, desired[i].uid,
                                     desired[i].user_manager,
                                     desired[i].unit);
            if (index >= 0)
                manager->units[index].seen_generation = generation;
            failures++;
        }
        if (dedicated <= 0) {
            desired[i] = desired[nr_desired - 1];
            nr_desired--;
        }
    }

    for (int i = 0; i < nr_desired; i++) {
        DesiredUnit *wanted = &desired[i];
        int index = find_managed(manager, wanted->uid, wanted->user_manager,
                                 wanted->unit);
        ManagedUnit *managed = NULL;
        if (index < 0) {
            if (manager->nr_units >= MAX_MANAGED_UNITS) {
                failures++;
                discovery_incomplete = true;
                continue;
            }
            sd_bus *bus = open_manager_bus(wanted->user_manager, wanted->uid);
            if (!bus) {
                failures++;
                continue;
            }
            managed = &manager->units[manager->nr_units];
            memset(managed, 0, sizeof(*managed));
            managed->uid = wanted->uid;
            managed->user_manager = wanted->user_manager;
            snprintf(managed->unit, sizeof(managed->unit), "%s",
                     wanted->unit);
            int result = read_unit_properties(
                bus, managed->unit, &managed->original_weight,
                &managed->original_allowed_cpus);
            sd_bus_unref(bus);
            if (result < 0) {
                log_warn("cgroup: cannot snapshot properties for %s",
                         managed->unit);
                failures++;
                continue;
            }
            bool was_dirty = manager->state_dirty;
            manager->nr_units++;
            managed->seen_generation = generation;
            manager->state_dirty = true;
            /* Save the original values before performing the first mutation. */
            if (persist_state(manager) != 0) {
                manager->nr_units--;
                manager->state_dirty = was_dirty;
                failures++;
                continue;
            }
            manager->state_dirty = false;
        } else {
            managed = &manager->units[index];
            managed->seen_generation = generation;
        }

        bool class_changed =
            strcmp(managed->class_name, wanted->klass->name) != 0;
        bool verify_due = !class_changed &&
            (managed->last_verified_ms == 0 ||
             now - managed->last_verified_ms >= PROPERTY_VERIFY_INTERVAL_MS);
        bool needs_apply = class_changed;
        sd_bus *bus = NULL;
        if (verify_due) {
            bus = open_manager_bus(managed->user_manager, managed->uid);
            uint64_t current_weight = 0;
            uint64_t current_allowed = 0;
            int result = bus ? read_unit_properties(
                                   bus, managed->unit, &current_weight,
                                   &current_allowed)
                             : -1;
            if (result < 0) {
                sd_bus_unref(bus);
                failures++;
                continue;
            }
            needs_apply = !unit_properties_match(
                current_weight, current_allowed, wanted->klass);
        }
        if (needs_apply) {
            if (!bus)
                bus = open_manager_bus(managed->user_manager, managed->uid);
            bool reasserting = !class_changed;
            int result = bus ? set_unit_properties(
                                   bus, managed->unit,
                                   (uint64_t)wanted->klass->cpu_weight,
                                   wanted->klass->cpu_mask,
                                   wanted->klass->cpu_mask != 0)
                             : -1;
            if (result < 0) {
                sd_bus_unref(bus);
                failures++;
                continue;
            }
            if (reasserting)
                log_warn("cgroup: reasserted externally changed unit %s",
                         managed->unit);
            snprintf(managed->class_name, sizeof(managed->class_name), "%s",
                     wanted->klass->name);
            log_info("cgroup: unit %s class=%s", managed->unit,
                     managed->class_name);
        }
        if (class_changed || verify_due) managed->last_verified_ms = now;
        sd_bus_unref(bus);
    }

    for (int i = manager->nr_units - 1; i >= 0; i--) {
        ManagedUnit *managed = &manager->units[i];
        if (managed->seen_generation == generation) continue;
        if (discovery_incomplete) continue;
        if (restore_unit(managed) != 0) {
            failures++;
            continue;
        }
        manager->units[i] = manager->units[manager->nr_units - 1];
        manager->nr_units--;
        manager->state_dirty = true;
    }
    if (manager->state_dirty) {
        if (persist_state(manager) != 0)
            failures++;
        else
            manager->state_dirty = false;
    }
    return failures == 0 ? 0 : -1;
#endif
}

bool cgroup_manager_is_available(const CgroupManager *manager) {
    return manager && manager->available;
}

int cgroup_manager_managed_count(const CgroupManager *manager) {
    return manager ? manager->nr_units : 0;
}
