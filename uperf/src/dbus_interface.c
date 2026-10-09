#define _GNU_SOURCE
#include "dbus_interface.h"
#include "log.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <gio/gio.h>
#include <polkit/polkit.h>

/* ----------------------------------------------------------------
 * DbusManager internal state
 * ---------------------------------------------------------------- */

struct DbusManager {
    gint              ref_count;
    gboolean          shutting_down;
    GBusType          bus_type;
    guint             owner_id;
    guint             export_id;
    guint             stats_timer;

    /* Properties */
    char             *current_mode;
    char             *current_scene;
    double             freqs[DBUS_MAX_CLUSTERS];
    int                nr_freqs;
    double             loads[DBUS_MAX_CPUS];
    int                nr_loads;
    gboolean           heavy_load;
    pid_t              active_pid;

    /* Bus connection for signal emission */
    GDBusConnection  *bus_conn;
    GDBusNodeInfo    *node_info;

    /* Game processes */
    GameProcessEntry  *games;
    int                nr_games;
    int                games_cap;

    /* Thermal state */
    int                max_temp_millidegC;
    char               thermal_state_str[32];

    /* Mode change handler */
    DbusSetModeFunc    set_mode_cb;
    void              *set_mode_ud;
    DbusReloadConfigFunc reload_config_cb;
    void              *reload_config_ud;
    DbusSetGameModeFunc set_game_mode_cb;
    void               *set_game_mode_ud;
    DbusSetManualFreqFunc set_manual_freq_cb;
    void               *set_manual_freq_ud;
    DbusSetActivePidFunc set_active_pid_cb;
    void               *set_active_pid_ud;

    /* Authorization. Production system-bus calls use Polkit asynchronously;
     * tests may inject a deterministic callback. */
    PolkitAuthority    *polkit_authority;
    GCancellable       *authorization_cancel;
    DbusAuthorizeFunc   authorize_cb;
    void               *authorize_ud;

    /* Manual frequency overrides */
    gint64             manual_freq[5];
    gboolean           manual_active;

    /* Task scheduler / cgroup status */
    int                tracked_processes;
    int                tracked_threads;
    struct {
        gint32 pid;
        char   comm[64];
        char   cgroup_class[64];
    }                 *workloads;
    int                nr_workloads;
    int                workloads_cap;
};

typedef struct {
    DbusManager          *manager;
    char                 *sender;
    char                 *action_id;
    char                 *method_name;
    GVariant             *parameters;
    GDBusMethodInvocation *invocation;
} PendingAuthorization;

/* DBus XML interface — embedded directly to avoid codegen step */
static const gchar introspection_xml[] =
    "<?xml version=\"1.0\"?>\n"
    "<node>\n"
    "  <interface name=\"org.uperflinux.Daemon\">\n"
    "    <property name=\"CurrentMode\" type=\"s\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"CurrentScene\" type=\"s\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"CpuFrequencies\" type=\"ad\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"CpuLoads\" type=\"ad\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"IsHeavyLoad\" type=\"b\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"GameProcesses\" type=\"a(issss)\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"MaxTemperature\" type=\"i\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"ThermalState\" type=\"s\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"ManualFreqOverride\" type=\"ax\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"SchedulerStatus\" type=\"(ii)\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"ManagedWorkloads\" type=\"a(iss)\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"false\"/>\n"
    "    </property>\n"
    "    <property name=\"ActiveProcess\" type=\"i\" access=\"read\">\n"
    "      <annotation name=\"org.freedesktop.DBus.Property.EmitsChangedSignal\" value=\"true\"/>\n"
    "    </property>\n"
    "    <method name=\"SetMode\">\n"
    "      <arg direction=\"in\" type=\"s\" name=\"mode\"/>\n"
    "      <arg direction=\"out\" type=\"b\" name=\"success\"/>\n"
    "    </method>\n"
    "    <method name=\"ReloadConfig\">\n"
    "      <arg direction=\"out\" type=\"b\" name=\"success\"/>\n"
    "    </method>\n"
    "    <method name=\"SetGameMode\">\n"
    "      <arg direction=\"in\" type=\"i\" name=\"pid\"/>\n"
    "      <arg direction=\"in\" type=\"s\" name=\"app\"/>\n"
    "      <arg direction=\"in\" type=\"s\" name=\"mode\"/>\n"
    "      <arg direction=\"out\" type=\"b\" name=\"success\"/>\n"
    "    </method>\n"
    "    <method name=\"SetManualFreq\">\n"
    "      <arg direction=\"in\" type=\"i\" name=\"cluster\"/>\n"
    "      <arg direction=\"in\" type=\"x\" name=\"freq_hz\"/>\n"
    "      <arg direction=\"out\" type=\"b\" name=\"success\"/>\n"
    "    </method>\n"
    "    <method name=\"SetActiveProcess\">\n"
    "      <arg direction=\"in\" type=\"i\" name=\"pid\"/>\n"
    "      <arg direction=\"out\" type=\"b\" name=\"success\"/>\n"
    "    </method>\n"
    "    <signal name=\"ModeChanged\">\n"
    "      <arg type=\"s\" name=\"mode\"/>\n"
    "    </signal>\n"
    "    <signal name=\"SceneChanged\">\n"
    "      <arg type=\"s\" name=\"scene\"/>\n"
    "    </signal>\n"
    "    <signal name=\"StatsUpdated\">\n"
    "      <arg type=\"ad\" name=\"frequencies\"/>\n"
    "      <arg type=\"ad\" name=\"loads\"/>\n"
    "    </signal>\n"
    "    <signal name=\"HeavyLoadStateChanged\">\n"
    "      <arg type=\"b\" name=\"active\"/>\n"
    "    </signal>\n"
    "    <signal name=\"ManualFreqChanged\">\n"
    "      <arg type=\"i\" name=\"cluster\"/>\n"
    "      <arg type=\"x\" name=\"freq_hz\"/>\n"
    "    </signal>\n"
    "    <signal name=\"ActiveProcessChanged\">\n"
    "      <arg type=\"i\" name=\"pid\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "</node>";

/* Helper: emit a DBus signal on the bus connection */
static void dbus_emit_signal(DbusManager *mgr, const char *signal_name, GVariant *params) {
    if (!mgr || !mgr->bus_conn) return;
    g_dbus_connection_emit_signal(mgr->bus_conn,
                                  NULL,              /* destination */
                                  "/org/uperflinux/Daemon", /* object path */
                                  "org.uperflinux.Daemon",  /* interface */
                                  signal_name,
                                  params,
                                  NULL);             /* callback */
}

static void game_process_entry_clear(GameProcessEntry *entry) {
    if (!entry) return;
    g_clear_pointer(&entry->comm, g_free);
    g_clear_pointer(&entry->cmdline, g_free);
    g_clear_pointer(&entry->package, g_free);
    g_clear_pointer(&entry->mode, g_free);
    entry->pid = 0;
}

static DbusManager *dbus_manager_ref_internal(DbusManager *mgr) {
    g_atomic_int_inc(&mgr->ref_count);
    return mgr;
}

static void dbus_manager_finalize(DbusManager *mgr) {
    g_clear_object(&mgr->polkit_authority);
    g_clear_object(&mgr->authorization_cancel);
    g_clear_pointer(&mgr->node_info, g_dbus_node_info_unref);
    g_clear_object(&mgr->bus_conn);
    g_free(mgr->current_mode);
    g_free(mgr->current_scene);
    if (mgr->games) {
        for (int i = 0; i < mgr->nr_games; i++)
            game_process_entry_clear(&mgr->games[i]);
        free(mgr->games);
    }
    free(mgr->workloads);
    free(mgr);
    log_debug("DBus manager destroyed");
}

static void dbus_manager_unref_internal(DbusManager *mgr) {
    if (g_atomic_int_dec_and_test(&mgr->ref_count))
        dbus_manager_finalize(mgr);
}

/* Handle SetMode after the caller has been authorized. */
static void handle_set_mode(DbusManager *mgr, GVariant *parameters,
                            GDBusMethodInvocation *invocation) {
    const char *mode;

    g_variant_get(parameters, "(&s)", &mode);
    log_info("DBus SetMode called: %s", mode);

    gboolean success = FALSE;
    if (mgr->set_mode_cb) {
        mgr->set_mode_cb(mode, mgr->set_mode_ud);
        success = TRUE;
    }

    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(b)", success));
}

/* Handle ReloadConfig after the caller has been authorized. */
static void handle_reload_config(DbusManager *mgr,
                                 GDBusMethodInvocation *invocation) {
    log_info("DBus ReloadConfig called");
    gboolean success = mgr->reload_config_cb &&
        mgr->reload_config_cb(mgr->reload_config_ud);
    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(b)", success));
}

static const char *authorization_action_for_method(const char *method_name) {
    if (strcmp(method_name, "SetMode") == 0 ||
        strcmp(method_name, "SetGameMode") == 0 ||
        strcmp(method_name, "SetActiveProcess") == 0)
        return DBUS_ACTION_CONTROL;
    if (strcmp(method_name, "SetManualFreq") == 0 ||
        strcmp(method_name, "ReloadConfig") == 0)
        return DBUS_ACTION_ADMIN;
    return NULL;
}

static void return_access_denied(GDBusMethodInvocation *invocation,
                                 const char *method_name) {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
        "Not authorized to call %s", method_name);
}

static gboolean get_sender_uid(DbusManager *mgr, const char *sender,
                               uid_t *uid) {
    if (!mgr || !mgr->bus_conn || !sender || !uid) return FALSE;

    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        mgr->bus_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixUser",
        g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    if (!reply) {
        log_warn("DBus: cannot resolve caller %s: %s", sender,
                 error ? error->message : "unknown error");
        g_clear_error(&error);
        return FALSE;
    }

    guint32 raw_uid = 0;
    g_variant_get(reply, "(u)", &raw_uid);
    g_variant_unref(reply);
    *uid = (uid_t)raw_uid;
    return TRUE;
}

/* PID-targeting operations stay passwordless for the active desktop user,
 * but the root daemon must not let that user retarget another user's tasks. */
static gboolean caller_may_target_pid(DbusManager *mgr, const char *sender,
                                      const char *method_name,
                                      GVariant *parameters,
                                      GDBusMethodInvocation *invocation) {
    if (mgr->bus_type != G_BUS_TYPE_SYSTEM ||
        (strcmp(method_name, "SetGameMode") != 0 &&
         strcmp(method_name, "SetActiveProcess") != 0))
        return TRUE;

    gint32 pid = 0;
    g_variant_get_child(parameters, 0, "i", &pid);
    if (pid <= 0) return TRUE;

    uid_t caller_uid = 0;
    if (!get_sender_uid(mgr, sender, &caller_uid)) {
        return_access_denied(invocation, method_name);
        return FALSE;
    }
    if (caller_uid == 0) return TRUE;

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/%d", pid);
    struct stat process_stat;
    if (stat(proc_path, &process_stat) != 0 ||
        process_stat.st_uid != caller_uid) {
        log_warn("DBus %s denied: caller uid %u does not own pid %d",
                 method_name, (unsigned int)caller_uid, pid);
        return_access_denied(invocation, method_name);
        return FALSE;
    }
    return TRUE;
}

/* Dispatch a state-changing method only after authorization succeeds. */
static void dispatch_authorized_method(DbusManager *mgr,
                                       const char *sender,
                                       const char *method_name,
                                       GVariant *parameters,
                                       GDBusMethodInvocation *invocation) {
    if (!caller_may_target_pid(mgr, sender, method_name, parameters,
                               invocation))
        return;

    if (strcmp(method_name, "SetMode") == 0) {
        const char *mode = NULL;
        g_variant_get(parameters, "(&s)", &mode);
        if (strcmp(mode, "balance") != 0 && strcmp(mode, "powersave") != 0 &&
            strcmp(mode, "performance") != 0) {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(b)", FALSE));
            return;
        }
        handle_set_mode(mgr, parameters, invocation);
        return;
    } else if (strcmp(method_name, "ReloadConfig") == 0) {
        handle_reload_config(mgr, invocation);
        return;
    } else if (strcmp(method_name, "SetGameMode") == 0) {
        int pid_in;
        const char *app_in, *mode_in;
        g_variant_get(parameters, "(i&s&s)", &pid_in, &app_in, &mode_in);
        gboolean valid = pid_in > 0 && app_in[0] != '\0' &&
            (strcmp(mode_in, "balance") == 0 ||
             strcmp(mode_in, "powersave") == 0 ||
             strcmp(mode_in, "performance") == 0);
        if (!valid) {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(b)", FALSE));
            return;
        }
        if (mgr->set_game_mode_cb &&
            !mgr->set_game_mode_cb((pid_t)pid_in, app_in, mode_in,
                                   mgr->set_game_mode_ud)) {
            g_dbus_method_invocation_return_value(
                invocation, g_variant_new("(b)", FALSE));
            return;
        }
        log_info("DBus SetGameMode called: pid=%d app=%s mode=%s", pid_in, app_in, mode_in);
        dbus_manager_set_game_mode(mgr, (pid_t)pid_in, app_in, mode_in);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", TRUE));
        return;
    } else if (strcmp(method_name, "SetManualFreq") == 0) {
        int cluster_in;
        gint64 freq_in;
        g_variant_get(parameters, "(ix)", &cluster_in, &freq_in);
        gboolean ok = (!mgr->set_manual_freq_cb ||
                       mgr->set_manual_freq_cb(cluster_in, freq_in,
                                               mgr->set_manual_freq_ud)) &&
                      dbus_manager_set_manual_freq(mgr, cluster_in, freq_in);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", ok));
        return;
    } else if (strcmp(method_name, "SetActiveProcess") == 0) {
        int pid_in = 0;
        g_variant_get(parameters, "(i)", &pid_in);
        gboolean ok = pid_in >= 0 && mgr->set_active_pid_cb &&
            mgr->set_active_pid_cb((pid_t)pid_in, mgr->set_active_pid_ud);
        if (ok) dbus_manager_set_active_pid(mgr, (pid_t)pid_in);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(b)", ok));
        return;
    }
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
        "Unknown method %s", method_name);
}

static void pending_authorization_free(PendingAuthorization *pending) {
    DbusManager *mgr = pending->manager;
    g_clear_object(&pending->invocation);
    g_clear_pointer(&pending->parameters, g_variant_unref);
    g_free(pending->method_name);
    g_free(pending->action_id);
    g_free(pending->sender);
    g_free(pending);
    dbus_manager_unref_internal(mgr);
}

static void authorization_check_done(GObject *source_object,
                                     GAsyncResult *result,
                                     gpointer user_data) {
    PendingAuthorization *pending = user_data;
    DbusManager *mgr = pending->manager;
    GError *error = NULL;
    PolkitAuthorizationResult *authorization =
        polkit_authority_check_authorization_finish(
            POLKIT_AUTHORITY(source_object), result, &error);

    if (mgr->shutting_down) {
        g_dbus_method_invocation_return_error_literal(
            pending->invocation, G_IO_ERROR, G_IO_ERROR_CANCELLED,
            "uperf-linux is shutting down");
    } else if (authorization &&
               polkit_authorization_result_get_is_authorized(authorization)) {
        dispatch_authorized_method(mgr, pending->sender, pending->method_name,
                                   pending->parameters, pending->invocation);
    } else {
        if (error) {
            log_warn("DBus authorization failed for %s (%s, sender %s): %s",
                     pending->method_name, pending->action_id, pending->sender,
                     error->message);
        } else {
            log_warn("DBus authorization denied for %s (%s, sender %s)",
                     pending->method_name, pending->action_id, pending->sender);
        }
        return_access_denied(pending->invocation, pending->method_name);
    }

    g_clear_error(&error);
    g_clear_object(&authorization);
    pending_authorization_free(pending);
}

/* Method dispatcher used by the exported GDBus object. */
static void handle_method_call(GDBusConnection      *connection,
                               const gchar          *sender,
                               const gchar          *object_path,
                               const gchar          *interface_name,
                               const gchar          *method_name,
                               GVariant             *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer              user_data) {
    (void)connection;
    (void)object_path;
    (void)interface_name;
    DbusManager *mgr = user_data;
    const char *action_id = authorization_action_for_method(method_name);

    if (!action_id) {
        g_dbus_method_invocation_return_error(
            invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
            "Unknown method %s", method_name);
        return;
    }

    if (mgr->authorize_cb) {
        GError *error = NULL;
        gboolean authorized = mgr->authorize_cb(
            sender, action_id, TRUE, &error, mgr->authorize_ud);
        if (!authorized) {
            log_warn("DBus authorization hook denied %s (%s): %s",
                     method_name, action_id,
                     error ? error->message : "not authorized");
            g_clear_error(&error);
            return_access_denied(invocation, method_name);
            return;
        }
        g_clear_error(&error);
        dispatch_authorized_method(mgr, sender, method_name, parameters,
                                   invocation);
        return;
    }

    /* Polkit's system-bus-name subject is meaningful only on the system bus.
     * Session-bus managers are used by the isolated unit-test backend. */
    if (mgr->bus_type != G_BUS_TYPE_SYSTEM) {
        dispatch_authorized_method(mgr, sender, method_name, parameters,
                                   invocation);
        return;
    }

    if (!mgr->polkit_authority || !sender || sender[0] != ':') {
        log_warn("DBus authorization unavailable for %s", method_name);
        return_access_denied(invocation, method_name);
        return;
    }

    PendingAuthorization *pending = g_new0(PendingAuthorization, 1);
    pending->manager = dbus_manager_ref_internal(mgr);
    pending->sender = g_strdup(sender);
    pending->action_id = g_strdup(action_id);
    pending->method_name = g_strdup(method_name);
    pending->parameters = g_variant_ref(parameters);
    pending->invocation = g_object_ref(invocation);

    PolkitSubject *subject = polkit_system_bus_name_new(sender);
    polkit_authority_check_authorization(
        mgr->polkit_authority, subject, action_id, NULL,
        POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
        mgr->authorization_cancel, authorization_check_done, pending);
    g_object_unref(subject);
}

/* ----------------------------------------------------------------
 * Property getters — return GVariant* for property query
 * ---------------------------------------------------------------- */

static GVariant *on_get_current_mode(DbusManager *mgr) {
    return g_variant_new_string(mgr->current_mode ? mgr->current_mode : "balance");
}

static GVariant *on_get_current_scene(DbusManager *mgr) {
    return g_variant_new_string(mgr->current_scene ? mgr->current_scene : "idle");
}

static GVariant *on_get_cpu_frequencies(DbusManager *mgr) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("ad"));
    for (int i = 0; i < mgr->nr_freqs; i++)
        g_variant_builder_add(&builder, "d", mgr->freqs[i]);
    return g_variant_builder_end(&builder);
}

static GVariant *on_get_cpu_loads(DbusManager *mgr) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("ad"));
    for (int i = 0; i < mgr->nr_loads; i++)
        g_variant_builder_add(&builder, "d", mgr->loads[i]);
    return g_variant_builder_end(&builder);
}

static G_GNUC_UNUSED GVariant *on_get_is_heavy_load(DbusManager *mgr) {
    return g_variant_new_boolean(mgr->heavy_load);
}

static GVariant *on_get_game_processes(DbusManager *mgr) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(issss)"));
    for (int i = 0; i < mgr->nr_games; i++) {
        g_variant_builder_add(&builder, "(issss)", mgr->games[i].pid,
                              mgr->games[i].comm ? mgr->games[i].comm : "",
                              mgr->games[i].cmdline ? mgr->games[i].cmdline : "",
                              mgr->games[i].package ? mgr->games[i].package : "",
                              mgr->games[i].mode ? mgr->games[i].mode : "balance");
    }
    return g_variant_builder_end(&builder);
}

static GVariant *on_get_max_temperature(DbusManager *mgr) {
    return g_variant_new_int32(mgr->max_temp_millidegC);
}

static GVariant *on_get_thermal_state(DbusManager *mgr) {
    return g_variant_new_string(mgr->thermal_state_str[0] ? mgr->thermal_state_str : "normal");
}

static GVariant *on_get_manual_freq_override(DbusManager *mgr) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("ax"));
    for (int i = 0; i < 5; i++) {
        g_variant_builder_add(&builder, "x", mgr->manual_freq[i]);
    }
    return g_variant_builder_end(&builder);
}

static GVariant *handle_get_property(GDBusConnection *connection,
                                     const gchar *sender,
                                     const gchar *object_path,
                                     const gchar *interface_name,
                                     const gchar *property_name,
                                     GError **error,
                                     gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    DbusManager *mgr = user_data;

    if (strcmp(property_name, "CurrentMode") == 0)
        return on_get_current_mode(mgr);
    if (strcmp(property_name, "CurrentScene") == 0)
        return on_get_current_scene(mgr);
    if (strcmp(property_name, "CpuFrequencies") == 0)
        return on_get_cpu_frequencies(mgr);
    if (strcmp(property_name, "CpuLoads") == 0)
        return on_get_cpu_loads(mgr);
    if (strcmp(property_name, "IsHeavyLoad") == 0)
        return on_get_is_heavy_load(mgr);
    if (strcmp(property_name, "GameProcesses") == 0)
        return on_get_game_processes(mgr);
    if (strcmp(property_name, "SchedulerStatus") == 0)
        return g_variant_new("(ii)", mgr->tracked_processes,
                             mgr->tracked_threads);
    if (strcmp(property_name, "ManagedWorkloads") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(iss)"));
        for (int i = 0; i < mgr->nr_workloads; i++)
            g_variant_builder_add(&b, "(iss)", mgr->workloads[i].pid,
                                  mgr->workloads[i].comm,
                                  mgr->workloads[i].cgroup_class);
        return g_variant_builder_end(&b);
    }
    if (strcmp(property_name, "MaxTemperature") == 0)
        return on_get_max_temperature(mgr);
    if (strcmp(property_name, "ThermalState") == 0)
        return on_get_thermal_state(mgr);
    if (strcmp(property_name, "ManualFreqOverride") == 0)
        return on_get_manual_freq_override(mgr);
    if (strcmp(property_name, "ActiveProcess") == 0)
        return g_variant_new_int32(mgr->active_pid);

    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                "Unknown property %s", property_name);
    return NULL;
}

static const GDBusInterfaceVTable interface_vtable = {
    .method_call = handle_method_call,
    .get_property = handle_get_property,
    .set_property = NULL,
};

/* ----------------------------------------------------------------
 * Stats update timer callback (emits StatsUpdated every 500ms)
 * ---------------------------------------------------------------- */

static gboolean stats_timer_callback(gpointer user_data) {
    DbusManager *mgr = (DbusManager *)user_data;

    GVariantBuilder freq_builder;
    g_variant_builder_init(&freq_builder, G_VARIANT_TYPE("ad"));
    for (int i = 0; i < mgr->nr_freqs; i++)
        g_variant_builder_add(&freq_builder, "d", mgr->freqs[i]);

    GVariantBuilder load_builder;
    g_variant_builder_init(&load_builder, G_VARIANT_TYPE("ad"));
    for (int i = 0; i < mgr->nr_loads; i++)
        g_variant_builder_add(&load_builder, "d", mgr->loads[i]);

    dbus_emit_signal(mgr, "StatsUpdated",
        g_variant_new("(@ad@ad)",
            g_variant_builder_end(&freq_builder),
            g_variant_builder_end(&load_builder)));

    return G_SOURCE_CONTINUE;
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

DbusManager *dbus_manager_new(GType bus_type) {
    DbusManager *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;

    mgr->ref_count = 1;
    mgr->bus_type = bus_type;
    mgr->current_mode = g_strdup("balance");
    mgr->current_scene = g_strdup("idle");
    mgr->heavy_load = FALSE;
    mgr->nr_freqs = 0;
    mgr->nr_loads = 0;
    mgr->nr_games = 0;
    mgr->games_cap = 16;
    mgr->games = calloc(mgr->games_cap, sizeof(GameProcessEntry));
    if (!mgr->games) {
        g_free(mgr->current_mode);
        g_free(mgr->current_scene);
        free(mgr);
        return NULL;
    }
    mgr->max_temp_millidegC = 0;
    mgr->thermal_state_str[0] = '\0';
    memset(mgr->manual_freq, 0, sizeof(mgr->manual_freq));
    mgr->manual_active = FALSE;

    /* Connect to bus synchronously */
    GError *err = NULL;
    mgr->bus_conn = g_bus_get_sync(bus_type, NULL, &err);
    if (!mgr->bus_conn) {
        log_error("DBus: failed to connect to bus: %s",
                  err ? err->message : "unknown error");
        g_clear_error(&err);
        dbus_manager_free(mgr);
        return NULL;
    }

    if (bus_type == G_BUS_TYPE_SYSTEM) {
        mgr->authorization_cancel = g_cancellable_new();
        mgr->polkit_authority = polkit_authority_get_sync(
            mgr->authorization_cancel, &err);
        if (!mgr->polkit_authority) {
            log_error("DBus: Polkit authority is unavailable; control methods "
                      "will be denied: %s",
                      err ? err->message : "unknown error");
            g_clear_error(&err);
        }
    }

    mgr->node_info = g_dbus_node_info_new_for_xml(introspection_xml, &err);
    if (!mgr->node_info) {
        log_error("DBus: invalid introspection XML: %s", err->message);
        g_clear_error(&err);
        dbus_manager_free(mgr);
        return NULL;
    }

    mgr->export_id = g_dbus_connection_register_object(
        mgr->bus_conn, "/org/uperflinux/Daemon",
        mgr->node_info->interfaces[0], &interface_vtable, mgr, NULL, &err);
    if (mgr->export_id == 0) {
        log_error("DBus: failed to export daemon object: %s", err->message);
        g_clear_error(&err);
        dbus_manager_free(mgr);
        return NULL;
    }

    GVariant *name_reply = g_dbus_connection_call_sync(
        mgr->bus_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName",
        g_variant_new("(su)", "org.uperflinux.Daemon", 4u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    guint32 name_result = 0;
    gboolean name_request_failed = name_reply == NULL;
    if (name_reply) {
        g_variant_get(name_reply, "(u)", &name_result);
        g_variant_unref(name_reply);
    }
    if (!name_reply && err) {
        log_error("DBus: failed to request service name: %s", err->message);
        g_clear_error(&err);
    }
    if (name_result != 1u && name_result != 4u) {
        if (!name_request_failed)
            log_error("DBus: service name request was rejected (result=%u)",
                      name_result);
        dbus_manager_free(mgr);
        return NULL;
    }
    mgr->owner_id = 1;
    mgr->stats_timer = g_timeout_add(500, stats_timer_callback, mgr);

    log_info("DBus manager exported on %s bus",
             bus_type == G_BUS_TYPE_SYSTEM ? "system" : "session");
    return mgr;
}

void dbus_manager_free(DbusManager *mgr) {
    if (!mgr || mgr->shutting_down) return;
    mgr->shutting_down = TRUE;
    if (mgr->authorization_cancel)
        g_cancellable_cancel(mgr->authorization_cancel);
    if (mgr->stats_timer) {
        g_source_remove(mgr->stats_timer);
        mgr->stats_timer = 0;
    }
    if (mgr->bus_conn && mgr->owner_id) {
        GError *err = NULL;
        GVariant *reply = g_dbus_connection_call_sync(
            mgr->bus_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ReleaseName",
            g_variant_new("(s)", "org.uperflinux.Daemon"),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
        if (reply) g_variant_unref(reply);
        g_clear_error(&err);
    }
    if (mgr->bus_conn && mgr->export_id)
        g_dbus_connection_unregister_object(mgr->bus_conn, mgr->export_id);
    mgr->export_id = 0;
    mgr->owner_id = 0;
    g_clear_pointer(&mgr->node_info, g_dbus_node_info_unref);
    g_clear_object(&mgr->bus_conn);
    dbus_manager_unref_internal(mgr);
}

void dbus_manager_set_mode(DbusManager *mgr, const char *mode) {
    if (!mgr || !mode) return;
    g_free(mgr->current_mode);
    mgr->current_mode = g_strdup(mode);
    log_info("DBus mode set to: %s", mode);

    /* Write to file for backward compatibility with CLI */
    FILE *fp = fopen("/run/uperf-linux/cur_powermode", "w");
    if (fp) {
        fprintf(fp, "%s\n", mode);
        fclose(fp);
    }

    /* Emit ModeChanged signal */
    dbus_emit_signal(mgr, "ModeChanged", g_variant_new("(s)", mode));
}

void dbus_manager_set_scene(DbusManager *mgr, const char *scene) {
    if (!mgr || !scene) return;
    g_free(mgr->current_scene);
    mgr->current_scene = g_strdup(scene);
    log_debug("DBus scene set to: %s", scene);
    dbus_emit_signal(mgr, "SceneChanged", g_variant_new("(s)", scene));
}

void dbus_manager_update_frequencies(DbusManager *mgr,
                                      const double *freqs,
                                      int nr_clusters) {
    if (!mgr || !freqs) return;
    int n = nr_clusters < DBUS_MAX_CLUSTERS ? nr_clusters : DBUS_MAX_CLUSTERS;
    memcpy(mgr->freqs, freqs, n * sizeof(double));
    mgr->nr_freqs = n;
}

void dbus_manager_update_loads(DbusManager *mgr,
                                const double *loads,
                                int nr_cpus) {
    if (!mgr || !loads) return;
    int n = nr_cpus < DBUS_MAX_CPUS ? nr_cpus : DBUS_MAX_CPUS;
    memcpy(mgr->loads, loads, n * sizeof(double));
    mgr->nr_loads = n;
}

void dbus_manager_set_heavy_load(DbusManager *mgr, gboolean active) {
    if (!mgr) return;
    if (mgr->heavy_load != active) {
        mgr->heavy_load = active;
        log_info("DBus heavy load state changed: %s", active ? "ACTIVE" : "inactive");
        dbus_emit_signal(mgr, "HeavyLoadStateChanged",
            g_variant_new("(b)", active));
    }
}

void dbus_manager_update_games(DbusManager *mgr,
                                const GameProcessEntry *processes,
                                int nr) {
    if (!mgr || nr < 0 || (nr > 0 && !processes)) return;

    if (nr > mgr->games_cap) {
        int old_cap = mgr->games_cap;
        int new_cap = nr <= G_MAXINT / 2 ? nr * 2 : nr;
        GameProcessEntry *new_arr = realloc(
            mgr->games, (size_t)new_cap * sizeof(*new_arr));
        if (!new_arr) {
            log_error("DBus: cannot grow game list from %d to %d entries",
                      old_cap, new_cap);
            return;
        }
        memset(new_arr + old_cap, 0,
               (size_t)(new_cap - old_cap) * sizeof(*new_arr));
        mgr->games = new_arr;
        mgr->games_cap = new_cap;
    }

    /* Clear entries removed by a shorter scan before lowering nr_games. */
    for (int i = nr; i < mgr->nr_games; i++)
        game_process_entry_clear(&mgr->games[i]);

    for (int i = 0; i < nr; i++) {
        GameProcessEntry replacement = {
            .pid = processes[i].pid,
            .comm = g_strdup(processes[i].comm),
            .cmdline = g_strdup(processes[i].cmdline),
            .package = g_strdup(processes[i].package),
            .mode = g_strdup(processes[i].mode),
        };
        game_process_entry_clear(&mgr->games[i]);
        mgr->games[i] = replacement;
    }
    mgr->nr_games = nr;
}

void dbus_manager_update_scheduler(DbusManager *mgr,
                                   int tracked_processes, int tracked_threads,
                                   const DbusWorkloadEntry *workloads, int nr) {
    if (!mgr || nr < 0 || (nr > 0 && !workloads)) return;

    mgr->tracked_processes = tracked_processes;
    mgr->tracked_threads = tracked_threads;

    if (nr > mgr->workloads_cap) {
        int new_cap = nr <= G_MAXINT / 2 ? nr * 2 : nr;
        void *grown = realloc(mgr->workloads,
                              (size_t)new_cap * sizeof(*mgr->workloads));
        if (!grown) {
            log_error("DBus: cannot grow workload list to %d entries", new_cap);
            return;
        }
        mgr->workloads = grown;
        mgr->workloads_cap = new_cap;
    }

    for (int i = 0; i < nr; i++) {
        mgr->workloads[i].pid = workloads[i].pid;
        g_strlcpy(mgr->workloads[i].comm, workloads[i].comm,
                  sizeof(mgr->workloads[i].comm));
        g_strlcpy(mgr->workloads[i].cgroup_class, workloads[i].cgroup_class,
                  sizeof(mgr->workloads[i].cgroup_class));
    }
    mgr->nr_workloads = nr;
}

const GameProcessEntry *dbus_manager_get_games(const DbusManager *mgr, int *nr) {
    if (!mgr || !nr) return NULL;
    *nr = mgr->nr_games;
    return mgr->games;
}

const char *dbus_manager_get_mode(const DbusManager *mgr) {
    return mgr ? mgr->current_mode : "balance";
}

const char *dbus_manager_get_scene(const DbusManager *mgr) {
    return mgr ? mgr->current_scene : "idle";
}

gboolean dbus_manager_emit_stats(DbusManager *mgr,
                                  const double *freqs, int nr_clusters,
                                  const double *loads, int nr_cpus) {
    if (!mgr) return FALSE;
    dbus_manager_update_frequencies(mgr, freqs, nr_clusters);
    dbus_manager_update_loads(mgr, loads, nr_cpus);
    return TRUE;
}

void dbus_manager_set_authorize_handler(DbusManager *mgr,
                                        DbusAuthorizeFunc callback,
                                        void *user_data) {
    if (!mgr) return;
    mgr->authorize_cb = callback;
    mgr->authorize_ud = user_data;
}

void dbus_manager_set_mode_handler(DbusManager *mgr,
                                    DbusSetModeFunc callback,
                                    void *user_data) {
    if (!mgr) return;
    mgr->set_mode_cb = callback;
    mgr->set_mode_ud = user_data;
}

void dbus_manager_set_reload_handler(DbusManager *mgr,
                                     DbusReloadConfigFunc callback,
                                     void *user_data) {
    if (!mgr) return;
    mgr->reload_config_cb = callback;
    mgr->reload_config_ud = user_data;
}

void dbus_manager_set_game_mode_handler(DbusManager *mgr,
                                         DbusSetGameModeFunc callback,
                                         void *user_data) {
    if (!mgr) return;
    mgr->set_game_mode_cb = callback;
    mgr->set_game_mode_ud = user_data;
}

void dbus_manager_set_manual_freq_handler(DbusManager *mgr,
                                           DbusSetManualFreqFunc callback,
                                           void *user_data) {
    if (!mgr) return;
    mgr->set_manual_freq_cb = callback;
    mgr->set_manual_freq_ud = user_data;
}

void dbus_manager_set_active_pid_handler(DbusManager *mgr,
                                         DbusSetActivePidFunc callback,
                                         void *user_data) {
    if (!mgr) return;
    mgr->set_active_pid_cb = callback;
    mgr->set_active_pid_ud = user_data;
}

void dbus_manager_set_active_pid(DbusManager *mgr, pid_t pid) {
    if (!mgr || pid < 0 || mgr->active_pid == pid) return;
    mgr->active_pid = pid;
    dbus_emit_signal(mgr, "ActiveProcessChanged",
                     g_variant_new("(i)", (gint)pid));
}

pid_t dbus_manager_get_active_pid(const DbusManager *mgr) {
    return mgr ? mgr->active_pid : 0;
}

void dbus_manager_set_thermal_state(DbusManager *mgr, int max_temp_millidegC,
                                     const char *state_str) {
    if (!mgr) return;
    mgr->max_temp_millidegC = max_temp_millidegC;
    if (state_str) {
        strncpy(mgr->thermal_state_str, state_str, sizeof(mgr->thermal_state_str) - 1);
        mgr->thermal_state_str[sizeof(mgr->thermal_state_str) - 1] = '\0';
    }
    log_debug("DBus thermal state: %d.%03d C %s",
              max_temp_millidegC / 1000, max_temp_millidegC % 1000,
              state_str ? state_str : "unknown");
}

void dbus_manager_set_game_mode(DbusManager *mgr, pid_t pid, const char *app_name,
                                 const char *mode) {
    if (!mgr || !app_name || !mode) return;
    log_info("DBus SetGameMode: pid=%d app='%s' mode='%s'", pid, app_name, mode);

    for (int i = 0; i < mgr->nr_games; i++) {
        if (mgr->games[i].pid == pid) {
            g_free(mgr->games[i].mode);
            mgr->games[i].mode = g_strdup(mode);
            return;
        }
    }
    if (mgr->nr_games < mgr->games_cap) {
        int idx = mgr->nr_games++;
        mgr->games[idx].pid = pid;
        mgr->games[idx].comm = g_strdup(app_name);
        mgr->games[idx].mode = g_strdup(mode);
    }
}

gboolean dbus_manager_set_manual_freq(DbusManager *mgr, int cluster, gint64 freq_hz) {
    if (!mgr) return FALSE;

    if (cluster < -1 || cluster > 3) {
        log_warn("Manual freq: invalid cluster %d", cluster);
        return FALSE;
    }

    if (freq_hz == 0) {
        int idx = cluster + 1;
        mgr->manual_freq[idx] = 0;
        mgr->manual_active = FALSE;
        for (int i = 0; i < 5; i++) {
            if (mgr->manual_freq[i] > 0) {
                mgr->manual_active = TRUE;
                break;
            }
        }
        log_info("Manual freq: released cluster %s (%d)",
                 cluster == -1 ? "GPU" : "CPU", cluster);
        dbus_emit_signal(mgr, "ManualFreqChanged",
                         g_variant_new("(ix)", cluster, freq_hz));
        return TRUE;
    }

    int idx = cluster + 1;
    mgr->manual_freq[idx] = freq_hz;
    mgr->manual_active = TRUE;
    log_info("Manual freq: cluster %s (%d) = %" G_GINT64_FORMAT " Hz (%.2f MHz)",
             cluster == -1 ? "GPU" : "CPU", cluster, freq_hz, freq_hz / 1e6);
    dbus_emit_signal(mgr, "ManualFreqChanged",
                     g_variant_new("(ix)", cluster, freq_hz));
    return TRUE;
}

gint64 dbus_manager_get_manual_freq(const DbusManager *mgr, int cluster) {
    if (!mgr) return 0;
    if (cluster < -1 || cluster > 3) return 0;
    return mgr->manual_freq[cluster + 1];
}
