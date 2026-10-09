#define _GNU_SOURCE

#include "config.h"
#include "log.h"
#include "task_scheduler.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t process_start_time(pid_t pid) {
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
        char *end = NULL;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (end == cursor) return 0;
        if (field == 22) return (uint64_t)value;
        cursor = end;
    }
    return 0;
}

static uint64_t current_affinity_mask(void) {
    cpu_set_t set;
    assert(sched_getaffinity(0, sizeof(set), &set) == 0);
    uint64_t mask = 0;
    for (int cpu = 0; cpu < MAX_CPUS && cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &set)) mask |= UINT64_C(1) << cpu;
    }
    return mask;
}

static void current_comm(char *comm, size_t size) {
    FILE *file = fopen("/proc/self/comm", "r");
    assert(file != NULL);
    assert(fgets(comm, size, file) != NULL);
    fclose(file);
    comm[strcspn(comm, "\r\n")] = '\0';
}

static void init_test_config(Config *config, bool game,
                             const char *thread_regex) {
    memset(config, 0, sizeof(*config));
    config->sched.enable = true;
    config->sched.nr_affinity_profiles = 1;
    strcpy(config->sched.affinity_profiles[0].name, "test");
    config->sched.nr_priority_profiles = 1;
    strcpy(config->sched.priority_profiles[0].name, "auto");
    config->sched.nr_rules = 1;
    strcpy(config->sched.rules[0].name, "test process");
    if (game) {
        strcpy(config->sched.rules[0].regex, ".*");
        config->sched.rules[0].match_game = true;
    } else {
        char comm[64];
        current_comm(comm, sizeof(comm));
        snprintf(config->sched.rules[0].regex,
                 sizeof(config->sched.rules[0].regex), "^%s$", comm);
        config->sched.rules[0].pinned = true;
    }
    config->sched.rules[0].nr_thread_rules = 1;
    strcpy(config->sched.rules[0].thread_rules[0].regex, thread_regex);
    config->sched.rules[0].thread_rules[0].affinity_index = 0;
    config->sched.rules[0].thread_rules[0].priority_index = 0;
}

static void test_context_mapping(void) {
    assert(task_scheduler_context(SCENE_IDLE, false, false) == SCHED_CTX_BG);
    assert(task_scheduler_context(SCENE_IDLE, true, false) == SCHED_CTX_IDLE);
    assert(task_scheduler_context(SCENE_TOUCH, true, false) ==
           SCHED_CTX_TOUCH);
    assert(task_scheduler_context(SCENE_TRIGGER, true, false) ==
           SCHED_CTX_TOUCH);
    assert(task_scheduler_context(SCENE_GESTURE, true, false) ==
           SCHED_CTX_TOUCH);
    assert(task_scheduler_context(SCENE_BOOST, true, false) ==
           SCHED_CTX_BOOST);
    assert(task_scheduler_context(SCENE_IDLE, false, true) == SCHED_CTX_IDLE);
}

static void test_priority_decode(void) {
    int policy = -1, nice_value = 99, rt_priority = 99;
    assert(task_scheduler_decode_priority(0, &policy, &nice_value,
                                          &rt_priority) == 1);
    assert(task_scheduler_decode_priority(-1, &policy, &nice_value,
                                          &rt_priority) == 0);
    assert(policy == SCHED_OTHER && nice_value == 0 && rt_priority == 0);
    assert(task_scheduler_decode_priority(-2, &policy, &nice_value,
                                          &rt_priority) == 0);
    assert(policy == SCHED_BATCH);
    assert(task_scheduler_decode_priority(-3, &policy, &nice_value,
                                          &rt_priority) == 0);
    assert(policy == SCHED_IDLE);
    assert(task_scheduler_decode_priority(98, &policy, &nice_value,
                                          &rt_priority) == 0);
    assert(policy == SCHED_FIFO && rt_priority == 98);
    assert(task_scheduler_decode_priority(100, &policy, &nice_value,
                                          &rt_priority) == 0);
    assert(policy == SCHED_OTHER && nice_value == -20);
    assert(task_scheduler_decode_priority(120, &policy, &nice_value,
                                          &rt_priority) == 0);
    assert(nice_value == 0);
    assert(task_scheduler_decode_priority(139, &policy, &nice_value,
                                          &rt_priority) == 0);
    assert(nice_value == 19);
    assert(task_scheduler_decode_priority(99, &policy, &nice_value,
                                          &rt_priority) < 0);
    assert(task_scheduler_decode_priority(140, &policy, &nice_value,
                                          &rt_priority) < 0);
}

static void test_live_affinity_apply_restore(void) {
    cpu_set_t original;
    assert(sched_getaffinity(0, sizeof(original), &original) == 0);
    int selected_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE && cpu < MAX_CPUS; cpu++) {
        if (CPU_ISSET(cpu, &original)) {
            selected_cpu = cpu;
            break;
        }
    }
    assert(selected_cpu >= 0);

    char comm[64];
    FILE *file = fopen("/proc/self/comm", "r");
    assert(file != NULL);
    assert(fgets(comm, sizeof(comm), file) != NULL);
    fclose(file);
    comm[strcspn(comm, "\r\n")] = '\0';

    Config config;
    memset(&config, 0, sizeof(config));
    config.sched.enable = true;
    config.sched.nr_affinity_profiles = 1;
    strcpy(config.sched.affinity_profiles[0].name, "test");
    config.sched.affinity_profiles[0].has_mask[SCHED_CTX_TOUCH] = true;
    config.sched.affinity_profiles[0].masks[SCHED_CTX_TOUCH] =
        UINT64_C(1) << selected_cpu;
    config.sched.nr_priority_profiles = 1;
    strcpy(config.sched.priority_profiles[0].name, "auto");
    config.sched.nr_rules = 1;
    strcpy(config.sched.rules[0].name, "self");
    snprintf(config.sched.rules[0].regex,
             sizeof(config.sched.rules[0].regex), "^%s$", comm);
    config.sched.rules[0].pinned = true;
    config.sched.rules[0].nr_thread_rules = 1;
    strcpy(config.sched.rules[0].thread_rules[0].regex, "/MAIN_THREAD/");
    strcpy(config.sched.rules[0].thread_rules[0].affinity_name, "test");
    config.sched.rules[0].thread_rules[0].affinity_index = 0;
    strcpy(config.sched.rules[0].thread_rules[0].priority_name, "auto");
    config.sched.rules[0].thread_rules[0].priority_index = 0;

    char state_path[128];
    snprintf(state_path, sizeof(state_path), "/tmp/uperf-sched-test-%d.state",
             getpid());
    unlink(state_path);
    TaskScheduler *scheduler = task_scheduler_new(&config, state_path);
    assert(scheduler != NULL);
    assert(task_scheduler_update(scheduler, NULL, 0, SCENE_TOUCH) == 0);
    assert(task_scheduler_tracked_processes(scheduler) == 1);
    assert(task_scheduler_tracked_threads(scheduler) == 1);

    cpu_set_t applied;
    assert(sched_getaffinity(0, sizeof(applied), &applied) == 0);
    assert(CPU_COUNT(&applied) == 1);
    assert(CPU_ISSET(selected_cpu, &applied));

    usleep(300000);
    assert(task_scheduler_update(scheduler, NULL, 0, SCENE_IDLE) == 0);
    cpu_set_t idle;
    assert(sched_getaffinity(0, sizeof(idle), &idle) == 0);
    assert(CPU_EQUAL(&original, &idle));

    usleep(300000);
    assert(task_scheduler_update(scheduler, NULL, 0, SCENE_TOUCH) == 0);
    assert(sched_getaffinity(0, sizeof(applied), &applied) == 0);
    assert(CPU_COUNT(&applied) == 1);
    assert(CPU_ISSET(selected_cpu, &applied));

    task_scheduler_free(scheduler);
    cpu_set_t restored;
    assert(sched_getaffinity(0, sizeof(restored), &restored) == 0);
    assert(CPU_EQUAL(&original, &restored));
    assert(access(state_path, F_OK) != 0 && errno == ENOENT);
}

static void test_crash_recovery(void) {
    cpu_set_t original;
    assert(sched_getaffinity(0, sizeof(original), &original) == 0);
    int selected_cpu = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE && cpu < MAX_CPUS; cpu++) {
        if (CPU_ISSET(cpu, &original)) {
            selected_cpu = cpu;
            break;
        }
    }
    assert(selected_cpu >= 0);

    int ready_pipe[2], stop_pipe[2];
    assert(pipe(ready_pipe) == 0);
    assert(pipe(stop_pipe) == 0);
    pid_t target = fork();
    assert(target >= 0);
    if (target == 0) {
        close(ready_pipe[0]);
        close(stop_pipe[1]);
        (void)prctl(PR_SET_NAME, "uperf-sched-tgt", 0, 0, 0);
        char byte = 'R';
        if (write(ready_pipe[1], &byte, 1) != 1) _exit(2);
        if (read(stop_pipe[0], &byte, 1) != 1) _exit(3);
        _exit(0);
    }
    close(ready_pipe[1]);
    close(stop_pipe[0]);
    char byte = 0;
    assert(read(ready_pipe[0], &byte, 1) == 1);
    close(ready_pipe[0]);

    Config config;
    memset(&config, 0, sizeof(config));
    config.sched.enable = true;
    config.sched.nr_affinity_profiles = 1;
    strcpy(config.sched.affinity_profiles[0].name, "test");
    config.sched.affinity_profiles[0].has_mask[SCHED_CTX_TOUCH] = true;
    config.sched.affinity_profiles[0].masks[SCHED_CTX_TOUCH] =
        UINT64_C(1) << selected_cpu;
    config.sched.nr_priority_profiles = 1;
    strcpy(config.sched.priority_profiles[0].name, "auto");
    config.sched.nr_rules = 1;
    strcpy(config.sched.rules[0].name, "crash target");
    strcpy(config.sched.rules[0].regex, "^uperf-sched-tgt$");
    config.sched.rules[0].pinned = true;
    config.sched.rules[0].nr_thread_rules = 1;
    strcpy(config.sched.rules[0].thread_rules[0].regex, "/MAIN_THREAD/");
    config.sched.rules[0].thread_rules[0].affinity_index = 0;
    config.sched.rules[0].thread_rules[0].priority_index = 0;

    char state_path[128];
    snprintf(state_path, sizeof(state_path),
             "/tmp/uperf-sched-crash-%d.state", getpid());
    unlink(state_path);
    pid_t tester = fork();
    assert(tester >= 0);
    if (tester == 0) {
        TaskScheduler *scheduler = task_scheduler_new(&config, state_path);
        if (!scheduler ||
            task_scheduler_update(scheduler, NULL, 0, SCENE_TOUCH) != 0 ||
            task_scheduler_tracked_processes(scheduler) != 1 ||
            task_scheduler_tracked_threads(scheduler) != 1)
            _exit(1);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(tester, &status, 0) == tester);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(access(state_path, F_OK) == 0);

    cpu_set_t applied;
    assert(sched_getaffinity(target, sizeof(applied), &applied) == 0);
    assert(CPU_COUNT(&applied) == 1);
    assert(CPU_ISSET(selected_cpu, &applied));

    /* Construction performs recovery before compiling the new policy. */
    TaskScheduler *recovery = task_scheduler_new(&config, state_path);
    assert(recovery != NULL);
    cpu_set_t restored;
    assert(sched_getaffinity(target, sizeof(restored), &restored) == 0);
    assert(CPU_EQUAL(&original, &restored));
    assert(access(state_path, F_OK) != 0 && errno == ENOENT);
    task_scheduler_free(recovery);

    assert(write(stop_pipe[1], "X", 1) == 1);
    close(stop_pipe[1]);
    assert(waitpid(target, &status, 0) == target);
    assert(WIFEXITED(status));
}

static void test_affinity_failure_is_reported(void) {
    uint64_t allowed = current_affinity_mask();
    int unavailable_cpu = -1;
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if ((allowed & (UINT64_C(1) << cpu)) == 0) {
            unavailable_cpu = cpu;
            break;
        }
    }
    if (unavailable_cpu < 0) return;

    Config config;
    init_test_config(&config, false, "/MAIN_THREAD/");
    config.sched.affinity_profiles[0].has_mask[SCHED_CTX_TOUCH] = true;
    config.sched.affinity_profiles[0].masks[SCHED_CTX_TOUCH] =
        UINT64_C(1) << unavailable_cpu;

    char state_path[128];
    snprintf(state_path, sizeof(state_path),
             "/tmp/uperf-sched-affinity-%d.state", getpid());
    unlink(state_path);
    TaskScheduler *scheduler = task_scheduler_new(&config, state_path);
    assert(scheduler != NULL);
    assert(task_scheduler_update(scheduler, NULL, 0, SCENE_TOUCH) == -1);
    task_scheduler_free(scheduler);
    assert(access(state_path, F_OK) != 0 && errno == ENOENT);
}

static void test_active_game_identity_uses_start_time(void) {
    Config config;
    init_test_config(&config, true, "/MAIN_THREAD/");
    uint64_t start_time = process_start_time(getpid());
    assert(start_time != 0);

    GameProcess game;
    memset(&game, 0, sizeof(game));
    game.pid = getpid();
    game.start_time = start_time + 1;

    TaskScheduler *scheduler = task_scheduler_new(&config, NULL);
    assert(scheduler != NULL);
    assert(task_scheduler_update(scheduler, &game, 1, SCENE_IDLE) == 0);
    assert(task_scheduler_get_active_pid(scheduler) == 0);
    assert(task_scheduler_tracked_processes(scheduler) == 0);

    game.start_time = start_time;
    assert(task_scheduler_update(scheduler, &game, 1, SCENE_IDLE) == 0);
    assert(task_scheduler_get_active_pid(scheduler) == getpid());
    assert(task_scheduler_tracked_processes(scheduler) == 1);
    task_scheduler_free(scheduler);
}

#define BATCH_WORKERS 12
static int worker_stop_pipe[2];

static void *wait_for_worker_stop(void *unused) {
    (void)unused;
    char byte = 0;
    assert(read(worker_stop_pipe[0], &byte, 1) == 1);
    return NULL;
}

static void test_new_threads_are_persisted_once(void) {
    assert(pipe(worker_stop_pipe) == 0);
    pthread_t workers[BATCH_WORKERS];
    for (int i = 0; i < BATCH_WORKERS; i++)
        assert(pthread_create(&workers[i], NULL, wait_for_worker_stop, NULL) ==
               0);

    Config config;
    init_test_config(&config, false, ".*");
    char state_path[128];
    snprintf(state_path, sizeof(state_path),
             "/tmp/uperf-sched-batch-%d.state", getpid());
    unlink(state_path);
    const char *state_name = strrchr(state_path, '/');
    assert(state_name != NULL);
    state_name++;

    int notify_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    assert(notify_fd >= 0);
    assert(inotify_add_watch(notify_fd, "/tmp", IN_MOVED_TO) >= 0);

    TaskScheduler *scheduler = task_scheduler_new(&config, state_path);
    assert(scheduler != NULL);
    assert(task_scheduler_update(scheduler, NULL, 0, SCENE_IDLE) == 0);
    assert(task_scheduler_tracked_threads(scheduler) >= BATCH_WORKERS + 1);

    char events[4096];
    int state_replacements = 0;
    ssize_t length = 0;
    while ((length = read(notify_fd, events, sizeof(events))) > 0) {
        for (char *cursor = events; cursor < events + length;) {
            struct inotify_event *event = (struct inotify_event *)cursor;
            if (event->len > 0 && strcmp(event->name, state_name) == 0)
                state_replacements++;
            cursor += sizeof(*event) + event->len;
        }
    }
    assert(length < 0 && errno == EAGAIN);
    assert(state_replacements == 1);

    close(notify_fd);
    task_scheduler_free(scheduler);
    for (int i = 0; i < BATCH_WORKERS; i++)
        assert(write(worker_stop_pipe[1], "X", 1) == 1);
    for (int i = 0; i < BATCH_WORKERS; i++)
        assert(pthread_join(workers[i], NULL) == 0);
    close(worker_stop_pipe[0]);
    close(worker_stop_pipe[1]);
    assert(access(state_path, F_OK) != 0 && errno == ENOENT);
}

static void test_invalid_recovery_attributes_are_rejected(void) {
    uint64_t start_time = process_start_time(getpid());
    uint64_t mask = current_affinity_mask();
    assert(start_time != 0 && mask != 0);

    char state_path[128];
    snprintf(state_path, sizeof(state_path),
             "/tmp/uperf-sched-invalid-%d.state", getpid());
    int fd = open(state_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    FILE *file = fdopen(fd, "w");
    assert(file != NULL);
    fprintf(file, "UPERF_SCHED_STATE 2\n");
    fprintf(file, "T %d %d %llu %016llx 999 0 0 0 0 0 0 0 1024\n",
            getpid(), getpid(), (unsigned long long)start_time,
            (unsigned long long)mask);
    assert(fclose(file) == 0);

    int original_policy = sched_getscheduler(0);
    assert(original_policy >= 0);
    Config config;
    memset(&config, 0, sizeof(config));
    TaskScheduler *scheduler = task_scheduler_new(&config, state_path);
    assert(scheduler != NULL);
    assert(sched_getscheduler(0) == original_policy);
    assert(access(state_path, F_OK) == 0);
    task_scheduler_free(scheduler);
    assert(access(state_path, F_OK) != 0 && errno == ENOENT);
}

int main(void) {
    log_init(UPERF_LOG_WARN, 0, NULL);
    test_context_mapping();
    test_priority_decode();
    test_live_affinity_apply_restore();
    test_crash_recovery();
    test_affinity_failure_is_reported();
    test_active_game_identity_uses_start_time();
    test_new_threads_are_persisted_once();
    test_invalid_recovery_attributes_are_rejected();
    log_shutdown();
    puts("task scheduler tests passed");
    return 0;
}
