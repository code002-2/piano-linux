#ifndef UPERF_CGROUP_MANAGER_H
#define UPERF_CGROUP_MANAGER_H

#include <stdbool.h>

#include "config.h"
#include "task_scheduler.h"

typedef struct CgroupManager CgroupManager;

/* systemd remains the cgroup-v2 single writer. The manager coalesces tracked
 * processes by their existing user/system unit, then applies CPUWeight and
 * AllowedCPUs through Manager.SetUnitProperties. It never migrates a PID out
 * of its owner unit. */
CgroupManager *cgroup_manager_new(const Config *config,
                                  const char *state_path);

/* Restore unit properties changed by this instance. */
void cgroup_manager_free(CgroupManager *manager);

/* Reconcile classified workloads with their existing systemd units. */
int cgroup_manager_update(CgroupManager *manager,
                          const TaskWorkload *workloads, int nr_workloads);

bool cgroup_manager_is_available(const CgroupManager *manager);
int cgroup_manager_managed_count(const CgroupManager *manager);

#endif /* UPERF_CGROUP_MANAGER_H */
