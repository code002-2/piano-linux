#ifndef UPERF_DBUS_INTERFACE_H
#define UPERF_DBUS_INTERFACE_H

#include <glib.h>
#include <glib-object.h>
#include <sys/types.h>

/* Maximum number of clusters and CPUs tracked */
#define DBUS_MAX_CLUSTERS  8
#define DBUS_MAX_CPUS      16

/* Polkit actions used to authorize state-changing D-Bus methods. */
#define DBUS_ACTION_CONTROL "org.uperflinux.control"
#define DBUS_ACTION_ADMIN   "org.uperflinux.admin"

/* Opaque DBus manager handle */
typedef struct DbusManager DbusManager;

/* Create and register the DBus manager.
 * bus_type: G_BUS_TYPE_SYSTEM or G_BUS_TYPE_SESSION
 * Returns NULL on failure. */
DbusManager *dbus_manager_new(GType bus_type);

/* Free DBus manager and unregister from bus. */
void dbus_manager_free(DbusManager *mgr);

/* Override the default Polkit authorization check. This is primarily useful
 * for tests and embedded session-bus users. A NULL callback restores the
 * default behavior (Polkit on the system bus, allow on a session bus).
 *
 * The callback runs before a method is dispatched. Returning FALSE rejects
 * the call with org.freedesktop.DBus.Error.AccessDenied. */
typedef gboolean (*DbusAuthorizeFunc)(const char *sender,
                                      const char *action_id,
                                      gboolean allow_user_interaction,
                                      GError **error,
                                      void *user_data);
void dbus_manager_set_authorize_handler(DbusManager *mgr,
                                        DbusAuthorizeFunc callback,
                                        void *user_data);

/* Update current power mode. Triggers ModeChanged signal.
 * mode: "balance", "powersave", or "performance" */
void dbus_manager_set_mode(DbusManager *mgr, const char *mode);

/* Update current scene state. Triggers SceneChanged signal.
 * scene: "idle", "touch", "trigger", "gesture", "junk", "switch", "boost" */
void dbus_manager_set_scene(DbusManager *mgr, const char *scene);

/* Update CPU frequency data (in MHz).
 * freqs: array of nr_clusters doubles
 * nr_clusters: number of clusters (max DBUS_MAX_CLUSTERS) */
void dbus_manager_update_frequencies(DbusManager *mgr,
                                      const double *freqs,
                                      int nr_clusters);

/* Update CPU load data (percent 0-100).
 * loads: array of nr_cpus doubles
 * nr_cpus: number of CPUs (max DBUS_MAX_CPUS) */
void dbus_manager_update_loads(DbusManager *mgr,
                                const double *loads,
                                int nr_cpus);

/* Update heavy load state. Triggers HeavyLoadStateChanged signal. */
void dbus_manager_set_heavy_load(DbusManager *mgr, gboolean active);

/* Update game process list.
 * processes: array of GameProcessEntry
 * nr: number of entries */
typedef struct {
    pid_t  pid;
    gchar *comm;
    gchar *cmdline;
    gchar *package;
    gchar *mode;
} GameProcessEntry;

void dbus_manager_update_games(DbusManager *mgr,
                                const GameProcessEntry *processes,
                                int nr);

/* One managed process, for the ManagedWorkloads property. */
typedef struct {
    gint32 pid;
    char   comm[64];
    char   cgroup_class[64];
} DbusWorkloadEntry;

/* Publish task-scheduler status: tracked counts and the per-process cgroup
 * class assignments currently in effect. */
void dbus_manager_update_scheduler(DbusManager *mgr,
                                   int tracked_processes, int tracked_threads,
                                   const DbusWorkloadEntry *workloads, int nr);

/* Borrow the current in-memory game list for diagnostics/tests. The returned
 * pointer remains owned by the manager and is invalidated by the next update. */
const GameProcessEntry *dbus_manager_get_games(const DbusManager *mgr, int *nr);

/* Get the current mode string (for DBus property getter). */
const char *dbus_manager_get_mode(const DbusManager *mgr);

/* Get the current scene string. */
const char *dbus_manager_get_scene(const DbusManager *mgr);

/* Called periodically (e.g., every 500ms) to emit StatsUpdated signal.
 * freqs: per-cluster frequency in MHz
 * loads: per-CPU load percentage
 * Returns TRUE if signal was emitted. */
gboolean dbus_manager_emit_stats(DbusManager *mgr,
                                  const double *freqs, int nr_clusters,
                                  const double *loads, int nr_cpus);

/* Called from main.c during initialization to connect the mode setter
 * to the daemon's internal mode-changing logic.
 * set_mode_callback: function pointer that actually changes the mode.
 * user_data: opaque pointer passed to callback. */
typedef void (*DbusSetModeFunc)(const char *mode, void *user_data);
void dbus_manager_set_mode_handler(DbusManager *mgr,
                                    DbusSetModeFunc callback,
                                    void *user_data);

typedef gboolean (*DbusReloadConfigFunc)(void *user_data);
void dbus_manager_set_reload_handler(DbusManager *mgr,
                                     DbusReloadConfigFunc callback,
                                     void *user_data);

typedef gboolean (*DbusSetGameModeFunc)(pid_t pid, const char *app_name,
                                        const char *mode, void *user_data);
void dbus_manager_set_game_mode_handler(DbusManager *mgr,
                                         DbusSetGameModeFunc callback,
                                         void *user_data);

typedef gboolean (*DbusSetManualFreqFunc)(int cluster, gint64 freq_hz,
                                           void *user_data);
void dbus_manager_set_manual_freq_handler(DbusManager *mgr,
                                           DbusSetManualFreqFunc callback,
                                           void *user_data);

typedef gboolean (*DbusSetActivePidFunc)(pid_t pid, void *user_data);
void dbus_manager_set_active_pid_handler(DbusManager *mgr,
                                         DbusSetActivePidFunc callback,
                                         void *user_data);
void dbus_manager_set_active_pid(DbusManager *mgr, pid_t pid);
pid_t dbus_manager_get_active_pid(const DbusManager *mgr);

/* Update thermal state information. */
void dbus_manager_set_thermal_state(DbusManager *mgr, int max_temp_millidegC,
                                     const char *state_str);

/* Set per-app mode for a detected game process. */
void dbus_manager_set_game_mode(DbusManager *mgr, pid_t pid, const char *app_name,
                                 const char *mode);

/* Manual frequency override — set CPU/GPU frequency directly.
 * cluster: 0=prime, 1=perf, 2=eff, 3=auto=all; -1=GPU
 * freq_hz: target frequency in Hz (0 = release override, resume auto)
 * Returns TRUE if override was accepted. */
gboolean dbus_manager_set_manual_freq(DbusManager *mgr, int cluster,
                                       gint64 freq_hz);

/* Get the current manual frequency override.
 * Returns the overridden frequency in Hz, or 0 if no override. */
gint64 dbus_manager_get_manual_freq(const DbusManager *mgr, int cluster);

#endif /* UPERF_DBUS_INTERFACE_H */
