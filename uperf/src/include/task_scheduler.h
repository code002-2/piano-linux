#ifndef UPERF_TASK_SCHEDULER_H
#define UPERF_TASK_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "config.h"
#include "game_scanner.h"
#include "state_machine.h"

typedef struct TaskScheduler TaskScheduler;

typedef struct {
    pid_t pid;
    uint64_t start_time;
    uid_t uid;
    bool active;
    bool game;
    char cgroup_class[MAX_NAME_LEN];
} TaskWorkload;

/* Create the scheduler, compile configured regexes and recover scheduler state
 * left by a previous daemon crash. */
TaskScheduler *task_scheduler_new(const Config *config,
                                  const char *state_path);

/* Restore every live thread changed by this instance and free resources. */
void task_scheduler_free(TaskScheduler *scheduler);

/* Select the process that receives foreground scene policies. pid=0 clears an
 * explicit selection and enables the single-detected-game fallback. */
int task_scheduler_set_active_pid(TaskScheduler *scheduler, pid_t pid);
pid_t task_scheduler_get_active_pid(const TaskScheduler *scheduler);
pid_t task_scheduler_get_requested_active_pid(const TaskScheduler *scheduler);

/* Reconcile process/thread rules with the live system. */
int task_scheduler_update(TaskScheduler *scheduler,
                          const GameProcess *games, int nr_games,
                          SceneState scene);

/* Return the currently classified process workloads for cgroup management. */
int task_scheduler_get_workloads(const TaskScheduler *scheduler,
                                 TaskWorkload *out, int capacity);

int task_scheduler_tracked_processes(const TaskScheduler *scheduler);
int task_scheduler_tracked_threads(const TaskScheduler *scheduler);

/* Pure helpers used by policy tests and by the executor. */
SchedContext task_scheduler_context(SceneState scene, bool active,
                                    bool pinned);
int task_scheduler_decode_priority(int encoded, int *policy, int *nice_value,
                                   int *rt_priority);

#endif /* UPERF_TASK_SCHEDULER_H */
