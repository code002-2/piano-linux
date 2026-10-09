#define _GNU_SOURCE

#include "cgroup_manager.h"
#include "log.h"

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int command_output(const char *command, char *output, size_t size) {
    FILE *pipe = popen(command, "r");
    if (!pipe) return -1;
    if (!fgets(output, size, pipe)) output[0] = '\0';
    int status = pclose(pipe);
    output[strcspn(output, "\r\n")] = '\0';
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static uint64_t process_start_time(pid_t pid) {
    char path[64], buffer[1024];
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
        char *end = NULL;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (end == cursor) return 0;
        if (field == 22) return (uint64_t)value;
        cursor = end;
    }
    return 0;
}

static uint64_t current_cpu_mask(void) {
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return 0;
    uint64_t mask = 0;
    for (int cpu = 0; cpu < MAX_CPUS && cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &allowed)) mask |= UINT64_C(1) << cpu;
    }
    return mask;
}

static pid_t find_nonleader_pid(const char *control_group, pid_t leader) {
    char path[512];
    snprintf(path, sizeof(path), "/sys/fs/cgroup%s/cgroup.procs",
             control_group);
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    pid_t pid = 0;
    pid_t selected = 0;
    while (fscanf(file, "%d", &pid) == 1) {
        if (pid != leader) {
            selected = pid;
            break;
        }
    }
    fclose(file);
    return selected;
}

static bool process_cgroup_contains(pid_t pid, const char *needle) {
    char path[64], line[512];
    snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle)) {
            found = true;
            break;
        }
    }
    fclose(file);
    return found;
}

static int read_property(const char *unit, const char *property, char *value,
                         size_t value_size) {
    char command[512];
    snprintf(command, sizeof(command),
             "systemctl --user show %s -p %s --value", unit, property);
    return command_output(command, value, value_size);
}

static int wait_property(const char *unit, const char *property,
                         const char *expected) {
    char value[128];
    for (int i = 0; i < 100; i++) {
        if (read_property(unit, property, value, sizeof(value)) == 0 &&
            strcmp(value, expected) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

int main(void) {
    char probe[64];
    if (command_output("systemctl --user is-system-running 2>/dev/null",
                       probe, sizeof(probe)) < 0 || probe[0] == '\0') {
        puts("SKIP: no systemd user manager");
        return 77;
    }

    log_init(UPERF_LOG_WARN, 0, NULL);
    char unit[96], command[512], state_path[160];
    snprintf(unit, sizeof(unit), "uperf-cgroup-test-%d.service", getpid());
    snprintf(state_path, sizeof(state_path),
             "/tmp/uperf-cgroup-test-%d.state", getpid());
    unlink(state_path);
    snprintf(command, sizeof(command),
             "systemd-run --user --unit=%s --collect --quiet "
             "/bin/sh -c 'sleep 60 & sleep 60 & wait'", unit);
    if (system(command) != 0) {
        puts("SKIP: cannot create transient user service");
        log_shutdown();
        return 77;
    }

    pid_t leader = 0;
    char control_group[384] = {0};
    pid_t child = 0;
    for (int i = 0; i < 100 && child == 0; i++) {
        snprintf(command, sizeof(command),
                 "systemctl --user show %s -p MainPID --value", unit);
        char value[64];
        if (command_output(command, value, sizeof(value)) == 0)
            leader = (pid_t)strtol(value, NULL, 10);
        snprintf(command, sizeof(command),
                 "systemctl --user show %s -p ControlGroup --value", unit);
        command_output(command, control_group, sizeof(control_group));
        if (leader > 0 && control_group[0] != '\0')
            child = find_nonleader_pid(control_group, leader);
        if (child == 0) usleep(20000);
    }
    if (child <= 0) {
        fprintf(stderr, "cannot find child process in %s\n", unit);
        goto fail;
    }

    uint64_t child_start = process_start_time(child);
    uint64_t leader_start = process_start_time(leader);
    char original_weight[128], original_cpus[128];
    if (child_start == 0 || leader_start == 0 ||
        read_property(unit, "CPUWeight", original_weight,
                      sizeof(original_weight)) != 0 ||
        read_property(unit, "AllowedCPUs", original_cpus,
                      sizeof(original_cpus)) != 0)
        goto fail;

    Config config;
    memset(&config, 0, sizeof(config));
    config.cgroup.enable = true;
    strcpy(config.cgroup.backend, "systemd");
    config.cgroup.nr_classes = 2;
    strcpy(config.cgroup.classes[0].name, "game");
    config.cgroup.classes[0].cpu_weight = 222;
    config.cgroup.classes[0].cpu_mask = current_cpu_mask();
    strcpy(config.cgroup.classes[1].name, "background");
    config.cgroup.classes[1].cpu_weight = 33;
    config.cgroup.classes[1].cpu_mask = current_cpu_mask();
    if (config.cgroup.classes[0].cpu_mask == 0) goto fail;

    TaskWorkload workloads[2] = {
        {.pid = child,
         .start_time = child_start,
         .uid = getuid(),
         .active = true,
         .game = true},
        {.pid = leader,
         .start_time = leader_start,
         .uid = getuid(),
         .active = false,
         .game = true},
    };
    strcpy(workloads[0].cgroup_class, "game");
    strcpy(workloads[1].cgroup_class, "background");

    /* A child alone does not own this service unit: its shell leader and
     * sibling are unrelated to that root. The cgroup layer must leave the
     * shared unit untouched while per-thread scheduling remains possible. */
    CgroupManager *shared = cgroup_manager_new(&config, state_path);
    if (!shared || cgroup_manager_update(shared, workloads, 1) != 0 ||
        cgroup_manager_managed_count(shared) != 0 ||
        wait_property(unit, "CPUWeight", original_weight) != 0) {
        cgroup_manager_free(shared);
        goto fail;
    }
    cgroup_manager_free(shared);

    /* Apply and exit without cleanup.  A second manager must restore the
     * original unit properties from the state journal. */
    pid_t tester = fork();
    if (tester == 0) {
        CgroupManager *manager = cgroup_manager_new(&config, state_path);
        if (!manager || !cgroup_manager_is_available(manager) ||
            cgroup_manager_update(manager, workloads, 2) != 0 ||
            cgroup_manager_managed_count(manager) != 1)
            _exit(1);
        _exit(0);
    }
    int status = 0;
    if (tester < 0 || waitpid(tester, &status, 0) < 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        wait_property(unit, "CPUWeight", "222") != 0 ||
        access(state_path, F_OK) != 0 ||
        !process_cgroup_contains(child, unit))
        goto fail;

    CgroupManager *manager = cgroup_manager_new(&config, state_path);
    if (!manager || !cgroup_manager_is_available(manager) ||
        wait_property(unit, "CPUWeight", original_weight) != 0 ||
        wait_property(unit, "AllowedCPUs", original_cpus) != 0 ||
        access(state_path, F_OK) == 0)
        goto fail_manager;

    if (cgroup_manager_update(manager, workloads, 2) != 0 ||
        cgroup_manager_managed_count(manager) != 1 ||
        wait_property(unit, "CPUWeight", "222") != 0)
        goto fail_manager;

    /* A stable reconcile must not replace and fsync the state journal. */
    struct stat state_before, state_after;
    if (stat(state_path, &state_before) != 0 ||
        cgroup_manager_update(manager, workloads, 2) != 0 ||
        stat(state_path, &state_after) != 0 ||
        state_before.st_ino != state_after.st_ino)
        goto fail_manager;

    /* If another controller changes a managed property, periodic verification
     * must put the selected class back without requiring a class transition. */
    snprintf(command, sizeof(command),
             "systemctl --user set-property --runtime %s CPUWeight=111", unit);
    if (system(command) != 0 || wait_property(unit, "CPUWeight", "111") != 0)
        goto fail_manager;
    usleep(5100000);
    if (cgroup_manager_update(manager, workloads, 2) != 0 ||
        wait_property(unit, "CPUWeight", "222") != 0)
        goto fail_manager;

    workloads[0].active = false;
    strcpy(workloads[0].cgroup_class, "background");
    if (cgroup_manager_update(manager, workloads, 2) != 0 ||
        wait_property(unit, "CPUWeight", "33") != 0)
        goto fail_manager;

    if (cgroup_manager_update(manager, NULL, 0) != 0 ||
        cgroup_manager_managed_count(manager) != 0 ||
        wait_property(unit, "CPUWeight", original_weight) != 0 ||
        wait_property(unit, "AllowedCPUs", original_cpus) != 0 ||
        !process_cgroup_contains(child, unit))
        goto fail_manager;
    cgroup_manager_free(manager);

    snprintf(command, sizeof(command), "systemctl --user stop %s", unit);
    if (system(command) != 0)
        fprintf(stderr, "warning: failed to stop test unit %s\n", unit);
    unlink(state_path);
    log_shutdown();
    puts("cgroup manager integration tests passed");
    return 0;

fail_manager:
    cgroup_manager_free(manager);
fail:
    snprintf(command, sizeof(command), "systemctl --user stop %s", unit);
    if (system(command) != 0)
        fprintf(stderr, "warning: failed to stop test unit %s\n", unit);
    unlink(state_path);
    log_shutdown();
    return 1;
}
