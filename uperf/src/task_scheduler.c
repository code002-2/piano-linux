#define _GNU_SOURCE

#include "task_scheduler.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef SCHED_FLAG_RESET_ON_FORK
#define SCHED_FLAG_RESET_ON_FORK 0x01
#endif
#ifndef SCHED_FLAG_UTIL_CLAMP_MIN
#define SCHED_FLAG_UTIL_CLAMP_MIN 0x20
#endif
#ifndef SCHED_FLAG_UTIL_CLAMP_MAX
#define SCHED_FLAG_UTIL_CLAMP_MAX 0x40
#endif
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif
#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif

#define DISCOVERY_INTERVAL_MS 1000
#define THREAD_SCAN_INTERVAL_MS 250
#define ERROR_LOG_INTERVAL_MS 5000
#define INITIAL_THREAD_CAPACITY 128

/* Local definition of the stable Linux sched_setattr ABI. This avoids the
 * struct sched_param collision between glibc <sched.h> and linux/sched/types.h
 * on older build distributions. */
struct uperf_sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
    uint32_t sched_util_min;
    uint32_t sched_util_max;
};

typedef struct {
    regex_t process_regex;
    bool process_regex_valid;
    regex_t thread_regex[MAX_THREAD_RULES];
    bool thread_regex_valid[MAX_THREAD_RULES];
} CompiledRule;

typedef struct {
    pid_t pid;
    uint64_t start_time;
    uid_t uid;
    int rule_index;
    bool game;
    bool pinned;
    bool active;
    int cgroup_index;
    uint64_t seen_generation;
    uint64_t thread_generation;
    char comm[64];
} TrackedProcess;

typedef struct {
    pid_t pid;
    pid_t tid;
    uint64_t start_time;
    int process_rule;
    int thread_rule;
    uint64_t seen_generation;

    cpu_set_t original_affinity;
    uint64_t original_affinity_mask;
    struct uperf_sched_attr original_attr;

    uint64_t applied_affinity_mask;
    int applied_priority;
    int applied_uclamp_min;
    int applied_uclamp_max;
    uint64_t failed_affinity_mask;
    uint64_t affinity_retry_after_ms;
    bool affinity_changed;
    bool scheduler_changed;
} TrackedThread;

struct TaskScheduler {
    SchedConfig sched;
    CgroupConfig cgroup;
    CompiledRule compiled[MAX_RULES];

    TrackedProcess processes[MAX_GAMES + MAX_RULES];
    int nr_processes;
    TrackedThread *threads;
    int nr_threads;
    int thread_capacity;

    pid_t requested_active_pid;
    uint64_t requested_active_start_time;
    pid_t effective_active_pid;
    uint64_t effective_active_start_time;
    bool force_discovery;
    bool state_dirty;
    uint64_t generation;
    uint64_t last_discovery_ms;
    uint64_t last_thread_scan_ms;
    uint64_t last_affinity_error_log_ms;
    uint64_t last_scheduler_error_log_ms;
    char state_path[MAX_PATH_LEN];
};

static int uperf_sched_getattr(pid_t tid, struct uperf_sched_attr *attr) {
#ifdef SYS_sched_getattr
    return (int)syscall(SYS_sched_getattr, tid, attr, sizeof(*attr), 0);
#else
    (void)tid;
    (void)attr;
    errno = ENOSYS;
    return -1;
#endif
}

static int uperf_sched_setattr(pid_t tid, struct uperf_sched_attr *attr) {
#ifdef SYS_sched_setattr
    return (int)syscall(SYS_sched_setattr, tid, attr, 0);
#else
    (void)tid;
    (void)attr;
    errno = ENOSYS;
    return -1;
#endif
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static bool numeric_name(const char *name) {
    if (!name || name[0] == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

static int read_text_file(const char *path, char *buffer, size_t size) {
    if (!path || !buffer || size == 0) return -1;
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

static uint64_t read_task_start_time(pid_t pid, pid_t tid) {
    char path[96];
    if (tid == pid)
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    else
        snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);
    char buffer[1024];
    if (read_text_file(path, buffer, sizeof(buffer)) < 0) return 0;

    char *cursor = strrchr(buffer, ')');
    if (!cursor || cursor[1] != ' ') return 0;
    cursor += 2; /* field 3: state */
    if (cursor[0] == '\0' || cursor[1] != ' ') return 0;
    cursor += 2; /* field 4 */
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

static bool read_process_identity(pid_t pid, char *comm, size_t comm_size,
                                  char *cmdline, size_t cmdline_size,
                                  uid_t *uid, uint64_t *start_time) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    if (read_text_file(path, comm, comm_size) < 0) return false;
    comm[strcspn(comm, "\r\n")] = '\0';

    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int length = read_text_file(path, cmdline, cmdline_size);
    if (length < 0) cmdline[0] = '\0';
    for (int i = 0; i < length; i++) {
        if (cmdline[i] == '\0') cmdline[i] = ' ';
    }

    snprintf(path, sizeof(path), "/proc/%d", pid);
    struct stat st;
    if (stat(path, &st) != 0) return false;
    *uid = st.st_uid;
    *start_time = read_task_start_time(pid, pid);
    return *start_time != 0;
}

static bool regex_matches(regex_t *regex, const char *first,
                          const char *second) {
    return (first && regexec(regex, first, 0, NULL, 0) == 0) ||
           (second && regexec(regex, second, 0, NULL, 0) == 0);
}

static bool pid_in_games(pid_t pid, uint64_t start_time,
                         const GameProcess *games, int nr_games) {
    for (int i = 0; games && i < nr_games; i++) {
        if (games[i].pid == pid && games[i].start_time == start_time)
            return true;
    }
    return false;
}

static int find_cgroup_index(const CgroupConfig *config, const char *name) {
    if (!config || !name || name[0] == '\0') return -1;
    for (int i = 0; i < config->nr_classes; i++) {
        if (strcmp(config->classes[i].name, name) == 0) return i;
    }
    return -1;
}

static int background_cgroup_index(const TaskScheduler *scheduler) {
    return find_cgroup_index(&scheduler->cgroup, "background");
}

SchedContext task_scheduler_context(SceneState scene, bool active,
                                    bool pinned) {
    if (!active && !pinned) return SCHED_CTX_BG;
    switch (scene) {
        case SCENE_BOOST:
            return SCHED_CTX_BOOST;
        case SCENE_TOUCH:
        case SCENE_TRIGGER:
        case SCENE_GESTURE:
        case SCENE_JUNK:
        case SCENE_SWITCH:
            return SCHED_CTX_TOUCH;
        case SCENE_IDLE:
        default:
            return SCHED_CTX_IDLE;
    }
}

int task_scheduler_decode_priority(int encoded, int *policy, int *nice_value,
                                   int *rt_priority) {
    if (!policy || !nice_value || !rt_priority) return -1;
    *nice_value = 0;
    *rt_priority = 0;
    if (encoded == 0) return 1; /* leave/restore original */
    if (encoded == -1) {
        *policy = SCHED_OTHER;
        return 0;
    }
    if (encoded == -2) {
        *policy = SCHED_BATCH;
        return 0;
    }
    if (encoded == -3) {
        *policy = SCHED_IDLE;
        return 0;
    }
    if (encoded >= 1 && encoded <= 98) {
        *policy = SCHED_FIFO;
        *rt_priority = encoded;
        return 0;
    }
    if (encoded >= 100 && encoded <= 139) {
        *policy = SCHED_OTHER;
        *nice_value = encoded - 120;
        return 0;
    }
    return -1;
}

static int compile_rules(TaskScheduler *scheduler) {
    for (int i = 0; i < scheduler->sched.nr_rules; i++) {
        const SchedRule *rule = &scheduler->sched.rules[i];
        CompiledRule *compiled = &scheduler->compiled[i];
        if (rule->regex[0] != '\0') {
            int result = regcomp(&compiled->process_regex, rule->regex,
                                 REG_EXTENDED | REG_NOSUB);
            if (result != 0) return -1;
            compiled->process_regex_valid = true;
        }
        for (int j = 0; j < rule->nr_thread_rules; j++) {
            if (strcmp(rule->thread_rules[j].regex, "/MAIN_THREAD/") == 0)
                continue;
            int result = regcomp(&compiled->thread_regex[j],
                                 rule->thread_rules[j].regex,
                                 REG_EXTENDED | REG_NOSUB);
            if (result != 0) return -1;
            compiled->thread_regex_valid[j] = true;
        }
    }
    return 0;
}

static void free_compiled_rules(TaskScheduler *scheduler) {
    for (int i = 0; i < scheduler->sched.nr_rules; i++) {
        CompiledRule *compiled = &scheduler->compiled[i];
        if (compiled->process_regex_valid)
            regfree(&compiled->process_regex);
        for (int j = 0; j < scheduler->sched.rules[i].nr_thread_rules; j++) {
            if (compiled->thread_regex_valid[j])
                regfree(&compiled->thread_regex[j]);
        }
    }
}

static int find_matching_process_rule(TaskScheduler *scheduler, pid_t pid,
                                      uint64_t start_time,
                                      const char *comm, const char *cmdline,
                                      bool game) {
    bool explicitly_active = pid == scheduler->effective_active_pid &&
        start_time == scheduler->effective_active_start_time;
    for (int i = 0; i < scheduler->sched.nr_rules; i++) {
        const SchedRule *rule = &scheduler->sched.rules[i];
        if (!rule->pinned && !game && !explicitly_active) continue;
        if (rule->match_game && !game) continue;
        if (scheduler->compiled[i].process_regex_valid &&
            !regex_matches(&scheduler->compiled[i].process_regex, comm,
                           cmdline))
            continue;
        return i;
    }
    return -1;
}

static int find_process(const TaskScheduler *scheduler, pid_t pid,
                        uint64_t start_time) {
    for (int i = 0; i < scheduler->nr_processes; i++) {
        if (scheduler->processes[i].pid == pid &&
            scheduler->processes[i].start_time == start_time)
            return i;
    }
    return -1;
}

static int find_thread(const TaskScheduler *scheduler, pid_t tid,
                       uint64_t start_time) {
    for (int i = 0; i < scheduler->nr_threads; i++) {
        if (scheduler->threads[i].tid == tid &&
            scheduler->threads[i].start_time == start_time)
            return i;
    }
    return -1;
}

static uint64_t cpuset_to_mask(const cpu_set_t *set) {
    uint64_t mask = 0;
    for (int cpu = 0; cpu < MAX_CPUS && cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, set)) mask |= UINT64_C(1) << cpu;
    }
    return mask;
}

static void mask_to_cpuset(uint64_t mask, cpu_set_t *set) {
    CPU_ZERO(set);
    for (int cpu = 0; cpu < MAX_CPUS && cpu < CPU_SETSIZE; cpu++) {
        if (mask & (UINT64_C(1) << cpu)) CPU_SET(cpu, set);
    }
}

static int restore_thread(TrackedThread *thread) {
    int result = 0;
    if (read_task_start_time(thread->pid, thread->tid) != thread->start_time)
        return 0;
    if (thread->affinity_changed &&
        sched_setaffinity(thread->tid, sizeof(thread->original_affinity),
                          &thread->original_affinity) != 0) {
        log_warn("sched: restore affinity for TID %d failed: %s", thread->tid,
                 strerror(errno));
        result = -1;
    }
    if (thread->scheduler_changed) {
        struct uperf_sched_attr original = thread->original_attr;
        original.size = sizeof(original);
        original.sched_flags &= SCHED_FLAG_RESET_ON_FORK;
        original.sched_flags |= SCHED_FLAG_UTIL_CLAMP_MIN |
                                SCHED_FLAG_UTIL_CLAMP_MAX;
        if (uperf_sched_setattr(thread->tid, &original) != 0) {
            log_warn("sched: restore attributes for TID %d failed: %s",
                     thread->tid, strerror(errno));
            result = -1;
        }
    }
    return result;
}

static int persist_state(const TaskScheduler *scheduler) {
    if (scheduler->state_path[0] == '\0') return 0;
    if (scheduler->nr_threads == 0) {
        if (unlink(scheduler->state_path) != 0 && errno != ENOENT)
            return -1;
        return 0;
    }
    char temp[MAX_PATH_LEN + 32];
    int length = snprintf(temp, sizeof(temp), "%s.tmp.%d",
                          scheduler->state_path, getpid());
    if (length <= 0 || length >= (int)sizeof(temp)) return -1;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    FILE *file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(temp);
        return -1;
    }
    fprintf(file, "UPERF_SCHED_STATE 2\n");
    for (int i = 0; i < scheduler->nr_threads; i++) {
        const TrackedThread *thread = &scheduler->threads[i];
        fprintf(file, "T %d %d %llu %016llx %u %d %u %llu %llu %llu "
                      "%llu %u %u\n",
                thread->pid, thread->tid,
                (unsigned long long)thread->start_time,
                (unsigned long long)thread->original_affinity_mask,
                thread->original_attr.sched_policy,
                thread->original_attr.sched_nice,
                thread->original_attr.sched_priority,
                (unsigned long long)thread->original_attr.sched_flags,
                (unsigned long long)thread->original_attr.sched_runtime,
                (unsigned long long)thread->original_attr.sched_deadline,
                (unsigned long long)thread->original_attr.sched_period,
                thread->original_attr.sched_util_min,
                thread->original_attr.sched_util_max);
    }
    bool ok = fflush(file) == 0 && fsync(fd) == 0;
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temp, scheduler->state_path) != 0) {
        unlink(temp);
        return -1;
    }
    return 0;
}

static bool valid_recovery_attr(const struct uperf_sched_attr *attr,
                                uint64_t affinity_mask) {
    if (!attr || affinity_mask == 0 || attr->sched_nice < -20 ||
        attr->sched_nice > 19 || attr->sched_util_min > 1024 ||
        attr->sched_util_max > 1024 ||
        attr->sched_util_min > attr->sched_util_max)
        return false;

    switch (attr->sched_policy) {
        case SCHED_OTHER:
        case SCHED_BATCH:
        case SCHED_IDLE:
        case SCHED_EXT:
            return attr->sched_priority == 0;
        case SCHED_FIFO:
        case SCHED_RR: {
            int minimum = sched_get_priority_min((int)attr->sched_policy);
            int maximum = sched_get_priority_max((int)attr->sched_policy);
            return minimum >= 0 && maximum >= minimum &&
                attr->sched_priority >= (uint32_t)minimum &&
                attr->sched_priority <= (uint32_t)maximum;
        }
        case SCHED_DEADLINE:
            return attr->sched_priority == 0 && attr->sched_runtime > 0 &&
                attr->sched_deadline >= attr->sched_runtime &&
                (attr->sched_period == 0 ||
                 attr->sched_period >= attr->sched_deadline);
        default:
            return false;
    }
}

static bool recovery_file_is_safe(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return st.st_uid == geteuid() && (st.st_mode & 0077) == 0;
}

static void recover_state_file(const char *path) {
    if (!path || path[0] == '\0') return;
    if (access(path, F_OK) != 0) return;
    if (!recovery_file_is_safe(path)) {
        log_warn("sched: refusing unsafe recovery state %s", path);
        return;
    }
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[512];
    int state_version = 0;
    if (fgets(line, sizeof(line), file)) {
        if (strcmp(line, "UPERF_SCHED_STATE 2\n") == 0)
            state_version = 2;
        else if (strcmp(line, "UPERF_SCHED_STATE 1\n") == 0)
            state_version = 1;
    }
    if (state_version == 0) {
        fclose(file);
        log_warn("sched: ignoring invalid recovery state %s", path);
        return;
    }
    int restored = 0;
    int failures = 0;
    while (fgets(line, sizeof(line), file)) {
        int pid = 0, tid = 0, nice_value = 0;
        unsigned int policy = 0, priority = 0, util_min = 0, util_max = 0;
        unsigned long long start = 0, mask = 0, flags = 0;
        unsigned long long runtime = 0, deadline = 0, period = 0;
        int fields = state_version == 2
            ? sscanf(line, "T %d %d %llu %llx %u %d %u %llu %llu %llu "
                           "%llu %u %u",
                     &pid, &tid, &start, &mask, &policy, &nice_value,
                     &priority, &flags, &runtime, &deadline, &period,
                     &util_min, &util_max)
            : sscanf(line, "T %d %d %llu %llx %u %d %u %llu %u %u",
                     &pid, &tid, &start, &mask, &policy, &nice_value,
                     &priority, &flags, &util_min, &util_max);
        int expected_fields = state_version == 2 ? 13 : 10;
        if (fields != expected_fields) {
            failures++;
            continue;
        }
        if (read_task_start_time((pid_t)pid, (pid_t)tid) != (uint64_t)start)
            continue;
        cpu_set_t original_set;
        mask_to_cpuset((uint64_t)mask, &original_set);
        struct uperf_sched_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);
        attr.sched_policy = policy;
        attr.sched_nice = nice_value;
        attr.sched_priority = priority;
        attr.sched_runtime = runtime;
        attr.sched_deadline = deadline;
        attr.sched_period = period;
        attr.sched_flags = (flags & SCHED_FLAG_RESET_ON_FORK) |
                           SCHED_FLAG_UTIL_CLAMP_MIN |
                           SCHED_FLAG_UTIL_CLAMP_MAX;
        attr.sched_util_min = util_min;
        attr.sched_util_max = util_max;
        if (!valid_recovery_attr(&attr, (uint64_t)mask)) {
            failures++;
            log_warn("sched: invalid recovery attributes for TID %d", tid);
            continue;
        }
        if (sched_setaffinity((pid_t)tid, sizeof(original_set),
                              &original_set) != 0 ||
            uperf_sched_setattr((pid_t)tid, &attr) != 0) {
            failures++;
            continue;
        }
        restored++;
    }
    fclose(file);
    if (failures == 0) unlink(path);
    log_info("sched: crash recovery restored %d thread(s), %d failure(s)",
             restored, failures);
}

static int ensure_thread_capacity(TaskScheduler *scheduler) {
    if (scheduler->nr_threads < scheduler->thread_capacity) return 0;
    int next = scheduler->thread_capacity == 0 ? INITIAL_THREAD_CAPACITY
                                               : scheduler->thread_capacity * 2;
    TrackedThread *threads = realloc(scheduler->threads,
                                     (size_t)next * sizeof(*threads));
    if (!threads) return -1;
    scheduler->threads = threads;
    scheduler->thread_capacity = next;
    return 0;
}

static TrackedThread *add_thread(TaskScheduler *scheduler, pid_t pid,
                                 pid_t tid, uint64_t start_time,
                                 int process_rule, int thread_rule) {
    if (ensure_thread_capacity(scheduler) != 0) return NULL;
    TrackedThread *thread = &scheduler->threads[scheduler->nr_threads];
    memset(thread, 0, sizeof(*thread));
    thread->pid = pid;
    thread->tid = tid;
    thread->start_time = start_time;
    thread->process_rule = process_rule;
    thread->thread_rule = thread_rule;
    thread->applied_priority = INT32_MIN;
    thread->applied_uclamp_min = -1;
    thread->applied_uclamp_max = -1;
    if (sched_getaffinity(tid, sizeof(thread->original_affinity),
                          &thread->original_affinity) != 0) {
        log_warn("sched: sched_getaffinity(%d) failed: %s", tid,
                 strerror(errno));
        return NULL;
    }
    thread->original_affinity_mask =
        cpuset_to_mask(&thread->original_affinity);
    memset(&thread->original_attr, 0, sizeof(thread->original_attr));
    thread->original_attr.size = sizeof(thread->original_attr);
    if (uperf_sched_getattr(tid, &thread->original_attr) != 0) {
        log_warn("sched: sched_getattr(%d) failed: %s", tid, strerror(errno));
        return NULL;
    }
    scheduler->nr_threads++;
    scheduler->state_dirty = true;
    return thread;
}

static int matching_thread_rule(TaskScheduler *scheduler, int process_rule,
                                pid_t pid, pid_t tid, const char *comm) {
    const SchedRule *rule = &scheduler->sched.rules[process_rule];
    const CompiledRule *compiled = &scheduler->compiled[process_rule];
    for (int i = 0; i < rule->nr_thread_rules; i++) {
        if (strcmp(rule->thread_rules[i].regex, "/MAIN_THREAD/") == 0) {
            if (pid == tid) return i;
            continue;
        }
        if (compiled->thread_regex_valid[i] &&
            regexec((regex_t *)&compiled->thread_regex[i], comm, 0, NULL, 0) == 0)
            return i;
    }
    return -1;
}

static int desired_cgroup_index(const TaskScheduler *scheduler,
                                const TrackedProcess *process) {
    if (process->game && !process->active) {
        int background = background_cgroup_index(scheduler);
        if (background >= 0) return background;
    }
    return process->cgroup_index;
}

static int apply_thread_policy(TaskScheduler *scheduler,
                               TrackedThread *thread,
                               const TrackedProcess *process,
                               SchedContext context) {
    const SchedRule *process_rule =
        &scheduler->sched.rules[thread->process_rule];
    const SchedThreadRule *rule =
        &process_rule->thread_rules[thread->thread_rule];
    uint64_t affinity_mask = 0;
    if (rule->affinity_index >= 0) {
        const AffinityProfile *profile =
            &scheduler->sched.affinity_profiles[rule->affinity_index];
        if (profile->has_mask[context]) affinity_mask = profile->masks[context];
    }
    int encoded_priority = 0;
    if (rule->priority_index >= 0) {
        encoded_priority = scheduler->sched
            .priority_profiles[rule->priority_index].values[context];
    }

    int uclamp_min = (int)thread->original_attr.sched_util_min;
    int uclamp_max = (int)thread->original_attr.sched_util_max;
    int cgroup_index = desired_cgroup_index(scheduler, process);
    if (scheduler->cgroup.enable && cgroup_index >= 0) {
        uclamp_min = scheduler->cgroup.classes[cgroup_index].uclamp_min;
        uclamp_max = scheduler->cgroup.classes[cgroup_index].uclamp_max;
    }

    int result = 0;
    if (affinity_mask != thread->applied_affinity_mask) {
        uint64_t now = monotonic_ms();
        if (thread->failed_affinity_mask == affinity_mask &&
            now < thread->affinity_retry_after_ms) {
            result = -1;
        } else {
            const cpu_set_t *desired = &thread->original_affinity;
            cpu_set_t configured;
            if (affinity_mask != 0) {
                mask_to_cpuset(affinity_mask, &configured);
                desired = &configured;
            }
            if (sched_setaffinity(thread->tid, sizeof(*desired), desired) != 0) {
                int error = errno;
                thread->failed_affinity_mask = affinity_mask;
                thread->affinity_retry_after_ms =
                    now + ERROR_LOG_INTERVAL_MS;
                result = -1;
                if (scheduler->last_affinity_error_log_ms == 0 ||
                    now - scheduler->last_affinity_error_log_ms >=
                        ERROR_LOG_INTERVAL_MS) {
                    log_warn("sched: set affinity TID %d mask=%016llx failed: "
                             "%s", thread->tid,
                             (unsigned long long)affinity_mask,
                             strerror(error));
                    scheduler->last_affinity_error_log_ms = now;
                }
            } else {
                thread->applied_affinity_mask = affinity_mask;
                thread->affinity_changed = affinity_mask != 0;
                thread->failed_affinity_mask = 0;
                thread->affinity_retry_after_ms = 0;
            }
        }
    } else {
        thread->failed_affinity_mask = 0;
        thread->affinity_retry_after_ms = 0;
    }

    if (encoded_priority == thread->applied_priority &&
        uclamp_min == thread->applied_uclamp_min &&
        uclamp_max == thread->applied_uclamp_max)
        return result;

    struct uperf_sched_attr desired = thread->original_attr;
    desired.size = sizeof(desired);
    desired.sched_flags &= SCHED_FLAG_RESET_ON_FORK;
    int policy = 0, nice_value = 0, rt_priority = 0;
    int decoded = task_scheduler_decode_priority(encoded_priority, &policy,
                                                  &nice_value, &rt_priority);
    if (decoded < 0) return -1;
    if (decoded == 0) {
        desired.sched_policy = (uint32_t)policy;
        desired.sched_nice = nice_value;
        desired.sched_priority = (uint32_t)rt_priority;
        desired.sched_runtime = 0;
        desired.sched_deadline = 0;
        desired.sched_period = 0;
        if (policy == SCHED_FIFO)
            desired.sched_flags |= SCHED_FLAG_RESET_ON_FORK;
    }
    desired.sched_flags |= SCHED_FLAG_UTIL_CLAMP_MIN |
                           SCHED_FLAG_UTIL_CLAMP_MAX;
    desired.sched_util_min = (uint32_t)uclamp_min;
    desired.sched_util_max = (uint32_t)uclamp_max;
    if (uperf_sched_setattr(thread->tid, &desired) != 0) {
        int error = errno;
        uint64_t now = monotonic_ms();
        if (scheduler->last_scheduler_error_log_ms == 0 ||
            now - scheduler->last_scheduler_error_log_ms >=
                ERROR_LOG_INTERVAL_MS) {
            log_warn("sched: set attributes TID %d priority=%d "
                     "uclamp=[%d,%d] failed: %s", thread->tid,
                     encoded_priority, uclamp_min, uclamp_max,
                     strerror(error));
            scheduler->last_scheduler_error_log_ms = now;
        }
        return -1;
    }
    thread->applied_priority = encoded_priority;
    thread->applied_uclamp_min = uclamp_min;
    thread->applied_uclamp_max = uclamp_max;
    thread->scheduler_changed = encoded_priority != 0 ||
        uclamp_min != (int)thread->original_attr.sched_util_min ||
        uclamp_max != (int)thread->original_attr.sched_util_max;
    return result;
}

static bool remove_thread(TaskScheduler *scheduler, int index, bool restore) {
    if (restore && restore_thread(&scheduler->threads[index]) != 0)
        return false;
    scheduler->threads[index] = scheduler->threads[scheduler->nr_threads - 1];
    scheduler->nr_threads--;
    scheduler->state_dirty = true;
    return true;
}

static int reconcile_process_threads(TaskScheduler *scheduler,
                                     TrackedProcess *process) {
    char path[96];
    snprintf(path, sizeof(path), "/proc/%d/task", process->pid);
    DIR *directory = opendir(path);
    if (!directory) return -1;
    uint64_t thread_generation = ++scheduler->generation;
    process->thread_generation = thread_generation;
    struct dirent *entry;
    int failures = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (!numeric_name(entry->d_name)) continue;
        pid_t tid = (pid_t)strtol(entry->d_name, NULL, 10);
        char comm_path[128];
        char comm[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/%d/task/%d/comm",
                 process->pid, tid);
        if (read_text_file(comm_path, comm, sizeof(comm)) < 0) continue;
        comm[strcspn(comm, "\r\n")] = '\0';
        int thread_rule = matching_thread_rule(scheduler, process->rule_index,
                                               process->pid, tid, comm);
        if (thread_rule < 0) continue;
        uint64_t start_time = read_task_start_time(process->pid, tid);
        if (start_time == 0) continue;
        int index = find_thread(scheduler, tid, start_time);
        TrackedThread *thread = NULL;
        if (index < 0) {
            thread = add_thread(scheduler, process->pid, tid, start_time,
                                process->rule_index, thread_rule);
            if (!thread) continue;
        } else {
            thread = &scheduler->threads[index];
            thread->process_rule = process->rule_index;
            thread->thread_rule = thread_rule;
        }
        thread->seen_generation = thread_generation;
    }
    closedir(directory);

    for (int i = scheduler->nr_threads - 1; i >= 0; i--) {
        TrackedThread *thread = &scheduler->threads[i];
        if (thread->pid == process->pid &&
            thread->seen_generation != thread_generation)
            if (!remove_thread(scheduler, i, true)) failures++;
    }
    return failures == 0 ? 0 : -1;
}

static int apply_process_thread_policies(TaskScheduler *scheduler,
                                         const TrackedProcess *process,
                                         SceneState scene) {
    int failures = 0;
    SchedContext context = task_scheduler_context(scene, process->active,
                                                   process->pinned);
    for (int i = 0; i < scheduler->nr_threads; i++) {
        TrackedThread *thread = &scheduler->threads[i];
        if (thread->pid != process->pid ||
            thread->seen_generation != process->thread_generation)
            continue;
        if (apply_thread_policy(scheduler, thread, process, context) != 0)
            failures++;
    }
    return failures == 0 ? 0 : -1;
}

static void remove_process(TaskScheduler *scheduler, int index) {
    pid_t pid = scheduler->processes[index].pid;
    for (int i = scheduler->nr_threads - 1; i >= 0; i--) {
        if (scheduler->threads[i].pid == pid)
            (void)remove_thread(scheduler, i, true);
    }
    scheduler->processes[index] =
        scheduler->processes[scheduler->nr_processes - 1];
    scheduler->nr_processes--;
}

static int discover_processes(TaskScheduler *scheduler,
                              const GameProcess *games, int nr_games) {
    DIR *proc = opendir("/proc");
    if (!proc) return -1;
    uint64_t process_generation = ++scheduler->generation;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
        if (!numeric_name(entry->d_name)) continue;
        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
        if (pid < 2) continue;
        char comm[64];
        char cmdline[4096];
        uid_t uid = 0;
        uint64_t start_time = 0;
        if (!read_process_identity(pid, comm, sizeof(comm), cmdline,
                                   sizeof(cmdline), &uid, &start_time))
            continue;
        bool game = pid_in_games(pid, start_time, games, nr_games);
        int rule_index = find_matching_process_rule(scheduler, pid, start_time,
                                                    comm, cmdline, game);
        if (rule_index < 0) continue;
        int index = find_process(scheduler, pid, start_time);
        TrackedProcess *process = NULL;
        if (index < 0) {
            if (scheduler->nr_processes >=
                (int)(sizeof(scheduler->processes) /
                      sizeof(scheduler->processes[0]))) {
                log_warn("sched: process tracking capacity reached");
                break;
            }
            process = &scheduler->processes[scheduler->nr_processes++];
            memset(process, 0, sizeof(*process));
            process->pid = pid;
            process->start_time = start_time;
            process->uid = uid;
        } else {
            process = &scheduler->processes[index];
        }
        process->rule_index = rule_index;
        process->game = game;
        process->pinned = scheduler->sched.rules[rule_index].pinned;
        process->active = pid == scheduler->effective_active_pid &&
            start_time == scheduler->effective_active_start_time;
        process->cgroup_index = find_cgroup_index(
            &scheduler->cgroup,
            scheduler->sched.rules[rule_index].cgroup_class);
        process->seen_generation = process_generation;
        snprintf(process->comm, sizeof(process->comm), "%s", comm);
    }
    closedir(proc);

    for (int i = scheduler->nr_processes - 1; i >= 0; i--) {
        if (scheduler->processes[i].seen_generation != process_generation)
            remove_process(scheduler, i);
    }
    return 0;
}

TaskScheduler *task_scheduler_new(const Config *config,
                                  const char *state_path) {
    if (!config) return NULL;
    TaskScheduler *scheduler = calloc(1, sizeof(*scheduler));
    if (!scheduler) return NULL;
    scheduler->sched = config->sched;
    scheduler->cgroup = config->cgroup;
    scheduler->force_discovery = true;
    if (state_path)
        snprintf(scheduler->state_path, sizeof(scheduler->state_path), "%s",
                 state_path);
    recover_state_file(scheduler->state_path);
    if (scheduler->sched.enable && compile_rules(scheduler) != 0) {
        free_compiled_rules(scheduler);
        free(scheduler);
        return NULL;
    }
    log_info("TaskScheduler created: enabled=%d rules=%d",
             scheduler->sched.enable, scheduler->sched.nr_rules);
    return scheduler;
}

void task_scheduler_free(TaskScheduler *scheduler) {
    if (!scheduler) return;
    for (int i = scheduler->nr_threads - 1; i >= 0; i--) {
        if (restore_thread(&scheduler->threads[i]) == 0)
            (void)remove_thread(scheduler, i, false);
    }
    persist_state(scheduler);
    free_compiled_rules(scheduler);
    free(scheduler->threads);
    free(scheduler);
}

int task_scheduler_set_active_pid(TaskScheduler *scheduler, pid_t pid) {
    if (!scheduler || pid < 0) return -1;
    uint64_t start_time = 0;
    if (pid > 0) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d", pid);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
        start_time = read_task_start_time(pid, pid);
        if (start_time == 0) return -1;
    }
    scheduler->requested_active_pid = pid;
    scheduler->requested_active_start_time = start_time;
    scheduler->force_discovery = true;
    return 0;
}

pid_t task_scheduler_get_active_pid(const TaskScheduler *scheduler) {
    return scheduler ? scheduler->effective_active_pid : 0;
}

pid_t task_scheduler_get_requested_active_pid(const TaskScheduler *scheduler) {
    return scheduler ? scheduler->requested_active_pid : 0;
}

int task_scheduler_update(TaskScheduler *scheduler,
                          const GameProcess *games, int nr_games,
                          SceneState scene) {
    if (!scheduler || nr_games < 0) return -1;
    if (nr_games > 0 && !games) return -1;
    if (!scheduler->sched.enable) return 0;
    if (scheduler->requested_active_pid > 0 &&
        read_task_start_time(scheduler->requested_active_pid,
                             scheduler->requested_active_pid) !=
            scheduler->requested_active_start_time) {
        scheduler->requested_active_pid = 0;
        scheduler->requested_active_start_time = 0;
        scheduler->force_discovery = true;
    }
    pid_t effective = scheduler->requested_active_pid;
    uint64_t effective_start = scheduler->requested_active_start_time;
    if (effective == 0 && nr_games == 1 && games[0].start_time != 0 &&
        read_task_start_time(games[0].pid, games[0].pid) ==
            games[0].start_time) {
        effective = games[0].pid;
        effective_start = games[0].start_time;
    }
    if (effective != scheduler->effective_active_pid ||
        effective_start != scheduler->effective_active_start_time) {
        scheduler->effective_active_pid = effective;
        scheduler->effective_active_start_time = effective_start;
        scheduler->force_discovery = true;
    }

    uint64_t now = monotonic_ms();
    if (scheduler->force_discovery || scheduler->last_discovery_ms == 0 ||
        now - scheduler->last_discovery_ms >= DISCOVERY_INTERVAL_MS) {
        if (discover_processes(scheduler, games, nr_games) != 0) return -1;
        scheduler->last_discovery_ms = now;
        scheduler->force_discovery = false;
    }
    int failures = 0;
    if (scheduler->last_thread_scan_ms == 0 ||
        now - scheduler->last_thread_scan_ms >= THREAD_SCAN_INTERVAL_MS) {
        for (int i = 0; i < scheduler->nr_processes; i++) {
            TrackedProcess *process = &scheduler->processes[i];
            process->active = process->pid == scheduler->effective_active_pid &&
                process->start_time == scheduler->effective_active_start_time;
            if (reconcile_process_threads(scheduler, process) != 0)
                failures++;
        }
        /* Retry restoration for threads whose process no longer matches. */
        for (int i = scheduler->nr_threads - 1; i >= 0; i--) {
            bool process_tracked = false;
            for (int j = 0; j < scheduler->nr_processes; j++) {
                if (scheduler->processes[j].pid == scheduler->threads[i].pid) {
                    process_tracked = true;
                    break;
                }
            }
            if (!process_tracked && !remove_thread(scheduler, i, true))
                failures++;
        }
        bool may_apply = true;
        if (scheduler->state_dirty) {
            if (persist_state(scheduler) != 0) {
                log_warn("sched: unable to persist recovery state: %s",
                         strerror(errno));
                failures++;
                may_apply = false;
            } else {
                scheduler->state_dirty = false;
            }
        }
        if (may_apply) {
            for (int i = 0; i < scheduler->nr_processes; i++) {
                if (apply_process_thread_policies(
                        scheduler, &scheduler->processes[i], scene) != 0)
                    failures++;
            }
        }
        scheduler->last_thread_scan_ms = now;
    } else if (scheduler->state_dirty) {
        if (persist_state(scheduler) == 0)
            scheduler->state_dirty = false;
        else
            failures++;
    }
    return failures == 0 ? 0 : -1;
}

int task_scheduler_get_workloads(const TaskScheduler *scheduler,
                                 TaskWorkload *out, int capacity) {
    if (!scheduler || !out || capacity < 0) return -1;
    int count = scheduler->nr_processes < capacity
        ? scheduler->nr_processes : capacity;
    for (int i = 0; i < count; i++) {
        const TrackedProcess *process = &scheduler->processes[i];
        TaskWorkload *workload = &out[i];
        memset(workload, 0, sizeof(*workload));
        workload->pid = process->pid;
        workload->start_time = process->start_time;
        workload->uid = process->uid;
        workload->active = process->active;
        workload->game = process->game;
        int cgroup_index = desired_cgroup_index(scheduler, process);
        if (cgroup_index >= 0)
            snprintf(workload->cgroup_class, sizeof(workload->cgroup_class),
                     "%s", scheduler->cgroup.classes[cgroup_index].name);
    }
    return count;
}

int task_scheduler_tracked_processes(const TaskScheduler *scheduler) {
    return scheduler ? scheduler->nr_processes : 0;
}

int task_scheduler_tracked_threads(const TaskScheduler *scheduler) {
    return scheduler ? scheduler->nr_threads : 0;
}
