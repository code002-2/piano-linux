#define _GNU_SOURCE
#include <glib.h>
#include <gio/gio.h>
#include "dbus_proxy.h"

#define UPERF_DBUS_PROXY(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), uperf_dbus_proxy_get_type(), DbusProxy))
#define DAEMON_BUS    "org.uperflinux.Daemon"
#define DAEMON_PATH   "/org/uperflinux/Daemon"
#define DAEMON_IFACE  "org.uperflinux.Daemon"
#define PRIVILEGED_CALL_TIMEOUT_MS 120000

G_DEFINE_TYPE(DbusProxy, uperf_dbus_proxy, G_TYPE_OBJECT)

static void uperf_dbus_proxy_init(DbusProxy *self) {
    self->current_mode = g_strdup("balance");
    self->current_scene = g_strdup("idle");
    self->thermal_state = g_strdup("normal");
    self->nr_freqs = 0;
    self->nr_loads = 0;
    self->is_heavy_load = FALSE;
    self->max_temp = 0;
    self->nr_games = 0;
}

static void proxy_dispose(GObject *obj) {
    DbusProxy *self = UPERF_DBUS_PROXY(obj);
    if (self->poll_id) { g_source_remove(self->poll_id); self->poll_id = 0; }
    if (self->stats_poll_id) { g_source_remove(self->stats_poll_id); self->stats_poll_id = 0; }
    g_clear_pointer(&self->bus, g_object_unref);
    g_free(self->current_mode); self->current_mode = NULL;
    g_free(self->current_scene); self->current_scene = NULL;
    g_free(self->thermal_state); self->thermal_state = NULL;
    g_free(self->manual_freq_str); self->manual_freq_str = NULL;
    for (int i = 0; i < self->nr_games; i++) {
        g_free(self->game_comm[i]);
        g_free(self->game_mode[i]);
    }
    g_free(self->game_comm); self->game_comm = NULL;
    g_free(self->game_pid); self->game_pid = NULL;
    g_free(self->game_mode); self->game_mode = NULL;
    for (int i = 0; i < self->nr_workloads; i++) {
        g_free(self->wl_comm[i]);
        g_free(self->wl_class[i]);
    }
    g_free(self->wl_pid); self->wl_pid = NULL;
    g_free(self->wl_comm); self->wl_comm = NULL;
    g_free(self->wl_class); self->wl_class = NULL;
    self->nr_workloads = 0;
    G_OBJECT_CLASS(uperf_dbus_proxy_parent_class)->dispose(obj);
}

static void proxy_finalize(GObject *obj) {
    G_OBJECT_CLASS(uperf_dbus_proxy_parent_class)->finalize(obj);
}

static void uperf_dbus_proxy_class_init(DbusProxyClass *klass) {
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->dispose  = proxy_dispose;
    obj_class->finalize = proxy_finalize;
}

static GDBusProxy* get_proxy(DbusProxy *self, GError **err) {
    if (!self->bus) {
        self->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, err);
        if (!self->bus) return NULL;
    }
    return g_dbus_proxy_new_sync(self->bus, G_DBUS_PROXY_FLAGS_NONE,
        NULL, DAEMON_BUS, DAEMON_PATH, DAEMON_IFACE, NULL, err);
}

static gboolean poll_properties(gpointer ud) {
    DbusProxy *self = (DbusProxy*)ud;
    GError *err = NULL;
    GDBusProxy *proxy = get_proxy(self, &err);
    if (!proxy) {
        g_clear_error(&err);
        return G_SOURCE_CONTINUE;
    }

    GVariant *m = g_dbus_proxy_get_cached_property(proxy, "CurrentMode");
    if (m) {
        gchar *old = self->current_mode;
        self->current_mode = g_variant_dup_string(m, NULL);
        g_free(old);
        g_variant_unref(m);
        if (self->mode_cb)
            ((void(*)(DbusProxy*,gchar*,gpointer))self->mode_cb)(self, self->current_mode, self->mode_ud);
    }

    GVariant *s = g_dbus_proxy_get_cached_property(proxy, "CurrentScene");
    if (s) {
        gchar *old = self->current_scene;
        self->current_scene = g_variant_dup_string(s, NULL);
        g_free(old);
        g_variant_unref(s);
        if (self->scene_cb)
            ((void(*)(DbusProxy*,gchar*,gpointer))self->scene_cb)(self, self->current_scene, self->scene_ud);
    }

    GVariant *hl = g_dbus_proxy_get_cached_property(proxy, "IsHeavyLoad");
    if (hl) {
        gboolean val = g_variant_get_boolean(hl);
        if (val != self->is_heavy_load) {
            self->is_heavy_load = val;
            if (self->heavy_cb)
                ((void(*)(DbusProxy*,gboolean,gpointer))self->heavy_cb)(self, val, self->heavy_ud);
        }
        g_variant_unref(hl);
    }

    GVariant *ft = g_dbus_proxy_get_cached_property(proxy, "MaxTemperature");
    if (ft) {
        self->max_temp = g_variant_get_int32(ft);
        if (self->thermal_cb)
            ((void(*)(DbusProxy*,gint32,gpointer))self->thermal_cb)(self, self->max_temp, self->thermal_ud);
        g_variant_unref(ft);
    }

    GVariant *ts = g_dbus_proxy_get_cached_property(proxy, "ThermalState");
    if (ts) {
        g_free(self->thermal_state);
        self->thermal_state = g_variant_dup_string(ts, NULL);
        g_variant_unref(ts);
    }

    GVariant *ap = g_dbus_proxy_get_cached_property(proxy, "ActiveProcess");
    if (ap) {
        self->active_pid = g_variant_get_int32(ap);
        g_variant_unref(ap);
    }

    GVariant *ss = g_dbus_proxy_get_cached_property(proxy, "SchedulerStatus");
    if (ss) {
        g_variant_get(ss, "(ii)", &self->tracked_processes,
                      &self->tracked_threads);
        g_variant_unref(ss);
    }

    GVariant *mw = g_dbus_proxy_get_cached_property(proxy, "ManagedWorkloads");
    if (mw) {
        for (int i = 0; i < self->nr_workloads; i++) {
            g_free(self->wl_comm[i]);
            g_free(self->wl_class[i]);
        }
        g_free(self->wl_pid);
        g_free(self->wl_comm);
        g_free(self->wl_class);

        gsize n = g_variant_n_children(mw);
        self->wl_pid   = g_malloc0((n + 1) * sizeof(gint));
        self->wl_comm  = g_malloc0((n + 1) * sizeof(gchar *));
        self->wl_class = g_malloc0((n + 1) * sizeof(gchar *));
        self->nr_workloads = (int)n;
        for (gsize i = 0; i < n; i++) {
            const gchar *comm = NULL, *cls = NULL;
            g_variant_get_child(mw, i, "(i&s&s)", &self->wl_pid[i], &comm, &cls);
            self->wl_comm[i]  = g_strdup(comm ? comm : "");
            self->wl_class[i] = g_strdup(cls ? cls : "");
        }
        g_variant_unref(mw);
    }

    g_object_unref(proxy);
    return G_SOURCE_CONTINUE;
}

static gboolean poll_stats(gpointer ud) {
    DbusProxy *self = (DbusProxy*)ud;
    GError *err = NULL;
    GDBusProxy *proxy = get_proxy(self, &err);
    if (!proxy) {
        g_clear_error(&err);
        return G_SOURCE_CONTINUE;
    }

    GVariant *fr = g_dbus_proxy_get_cached_property(proxy, "CpuFrequencies");
    if (fr) {
        gsize n;
        gdouble *arr = (gdouble*)g_variant_get_fixed_array(fr, &n, sizeof(gdouble));
        int cnt = n < 8 ? (int)n : 8;
        memcpy(self->freqs, arr, cnt * sizeof(gdouble));
        self->nr_freqs = cnt;
        g_variant_unref(fr);
    }

    GVariant *ld = g_dbus_proxy_get_cached_property(proxy, "CpuLoads");
    if (ld) {
        gsize n;
        gdouble *arr = (gdouble*)g_variant_get_fixed_array(ld, &n, sizeof(gdouble));
        int cnt = n < 8 ? (int)n : 8;
        memcpy(self->loads, arr, cnt * sizeof(gdouble));
        self->nr_loads = cnt;
        g_variant_unref(ld);
    }

    if (self->stats_cb)
        ((void(*)(DbusProxy*,gpointer))self->stats_cb)(self, self->stats_ud);

    GVariant *gp = g_dbus_proxy_get_cached_property(proxy, "GameProcesses");
    if (gp) {
        gsize n_entries = g_variant_n_children(gp);
        for (int i = 0; i < self->nr_games; i++) {
            g_free(self->game_comm[i]);
            g_free(self->game_mode[i]);
        }
        g_free(self->game_comm);
        g_free(self->game_pid);
        g_free(self->game_mode);

        int cap = (int)n_entries + 4;
        self->game_comm = g_malloc0(cap * sizeof(gchar*));
        self->game_pid  = g_malloc0(cap * sizeof(gint));
        self->game_mode = g_malloc0(cap * sizeof(gchar*));
        self->nr_games = (int)n_entries;
        for (gsize i = 0; i < n_entries; i++) {
            const gchar *comm = NULL;
            const gchar *cmdline = NULL;
            const gchar *package = NULL;
            const gchar *mode = NULL;
            g_variant_get_child(gp, i, "(i&s&s&s&s)",
                                &self->game_pid[i], &comm, &cmdline,
                                &package, &mode);
            (void)cmdline;
            (void)package;
            self->game_comm[i] = g_strdup(comm);
            self->game_mode[i] = g_strdup(mode);
        }
        g_variant_unref(gp);
    }

    g_object_unref(proxy);
    return G_SOURCE_CONTINUE;
}

void dbus_proxy_start_polling(DbusProxy *self) {
    self->poll_id = g_timeout_add(500, poll_properties, self);
    self->stats_poll_id = g_timeout_add(500, poll_stats, self);
}

void dbus_proxy_disconnect(DbusProxy *self) {
    if (self->poll_id) { g_source_remove(self->poll_id); self->poll_id = 0; }
    if (self->stats_poll_id) { g_source_remove(self->stats_poll_id); self->stats_poll_id = 0; }
}

void dbus_proxy_set_mode_cb(DbusProxy *self, GCallback cb, gpointer ud) { self->mode_cb = cb; self->mode_ud = ud; }
void dbus_proxy_set_scene_cb(DbusProxy *self, GCallback cb, gpointer ud) { self->scene_cb = cb; self->scene_ud = ud; }
void dbus_proxy_set_stats_cb(DbusProxy *self, GCallback cb, gpointer ud) { self->stats_cb = cb; self->stats_ud = ud; }
void dbus_proxy_set_heavy_cb(DbusProxy *self, GCallback cb, gpointer ud) { self->heavy_cb = cb; self->heavy_ud = ud; }
void dbus_proxy_set_thermal_cb(DbusProxy *self, GCallback cb, gpointer ud) { self->thermal_cb = cb; self->thermal_ud = ud; }

gboolean dbus_proxy_set_mode(DbusProxy *self, const gchar *mode) {
    GError *err = NULL;
    GDBusProxy *proxy = get_proxy(self, &err);
    if (!proxy) { g_clear_error(&err); return FALSE; }
    GVariant *ret = g_dbus_proxy_call_sync(proxy, "SetMode",
        g_variant_new("(s)", mode),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    g_object_unref(proxy);
    if (!ret) { g_warning("SetMode: %s", err->message); g_error_free(err); return FALSE; }
    gboolean ok; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret);
    return ok;
}

gboolean dbus_proxy_set_game_mode(DbusProxy *self, gint pid, const gchar *app, const gchar *mode) {
    GError *err = NULL;
    GDBusProxy *proxy = get_proxy(self, &err);
    if (!proxy) { g_clear_error(&err); return FALSE; }
    GVariant *ret = g_dbus_proxy_call_sync(proxy, "SetGameMode",
        g_variant_new("(iss)", pid, app, mode),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    g_object_unref(proxy);
    if (!ret) { g_clear_error(&err); return FALSE; }
    gboolean ok; g_variant_get(ret, "(b)", &ok); g_variant_unref(ret);
    return ok;
}

gboolean dbus_proxy_reload_config(DbusProxy *self) {
    GError *err = NULL;
    GDBusProxy *proxy = get_proxy(self, &err);
    if (!proxy) { g_clear_error(&err); return FALSE; }
    GVariant *ret = g_dbus_proxy_call_sync(proxy, "ReloadConfig", NULL,
        G_DBUS_CALL_FLAGS_NONE, PRIVILEGED_CALL_TIMEOUT_MS, NULL, &err);
    g_object_unref(proxy);
    if (!ret) {
        g_warning("ReloadConfig: %s", err ? err->message : "request failed");
        g_clear_error(&err);
        return FALSE;
    }
    gboolean accepted = FALSE;
    g_variant_get(ret, "(b)", &accepted);
    g_variant_unref(ret);
    return accepted;
}

gboolean dbus_proxy_apply_freq_override(DbusProxy *self,
    gint64 prime, gint64 perf, gint64 eff, gint64 gpu)
{
    GError *err = NULL;
    GDBusProxy *proxy = get_proxy(self, &err);
    if (!proxy) { g_clear_error(&err); return FALSE; }
    const gint clusters[] = {0, 1, 2, -1};
    const gint64 frequencies[] = {prime, perf, eff, gpu};
    gboolean success = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(clusters); i++) {
        GVariant *ret = g_dbus_proxy_call_sync(
            proxy, "SetManualFreq",
            g_variant_new("(ix)", clusters[i], frequencies[i]),
            G_DBUS_CALL_FLAGS_NONE, PRIVILEGED_CALL_TIMEOUT_MS, NULL, &err);
        gboolean accepted = FALSE;
        if (ret) {
            g_variant_get(ret, "(b)", &accepted);
            g_variant_unref(ret);
        }
        if (!accepted) {
            g_warning("SetManualFreq cluster %d failed: %s", clusters[i],
                      err ? err->message : "request rejected");
            success = FALSE;
            /* Avoid leaving a partial set when a later policy rejects the
             * requested frequency. */
            for (guint rollback = 0; rollback < i; rollback++) {
                GVariant *released = g_dbus_proxy_call_sync(
                    proxy, "SetManualFreq",
                    g_variant_new("(ix)", clusters[rollback], (gint64)0),
                    G_DBUS_CALL_FLAGS_NONE, PRIVILEGED_CALL_TIMEOUT_MS,
                    NULL, NULL);
                if (released) g_variant_unref(released);
            }
            g_clear_error(&err);
            break;
        }
        g_clear_error(&err);
    }
    g_object_unref(proxy);
    return success;
}

gboolean dbus_proxy_release_freq_override(DbusProxy *self) {
    return dbus_proxy_apply_freq_override(self, 0, 0, 0, 0);
}
