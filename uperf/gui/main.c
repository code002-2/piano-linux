#include <gtk/gtk.h>
#include <adwaita.h>
#include "dbus_proxy.h"

/* ----------------------------------------------------------------
 * uperf-linux GUI — rebuilt around libadwaita idioms.
 *
 * Navigation:  AdwViewStack + AdwViewSwitcher (responsive, with a
 *              selected-state that the old hand-rolled tab bar lacked).
 * Content:     each page groups related data into AdwPreferencesGroup
 *              cards; live values live in AdwActionRow suffixes so the
 *              layout stays stable as numbers change.
 *
 * The DbusProxy backend contract is unchanged (see dbus_proxy.h). All
 * daemon frequencies are reported in MHz; the manual-override API takes
 * CPU values in Hz and GPU in Hz.
 * ---------------------------------------------------------------- */

#define UPERF_MAX_CLUSTERS 8
#define UPERF_MAX_CPUS     8

/* Cluster labels used by the dashboard + override page. The daemon
 * publishes clusters ordered prime → performance → efficiency. */
static const char *const CLUSTER_NAMES[] = { "Prime", "Performance", "Efficiency" };

typedef struct {
    DbusProxy *proxy;

    /* Dashboard live rows */
    GtkWidget  *row_mode;
    GtkWidget  *row_scene;
    GtkWidget  *row_heavy;
    GtkWidget  *row_temp;
    GtkWidget  *row_thermal;
    GtkWidget  *temp_bar;
    GtkWidget  *freq_rows[UPERF_MAX_CLUSTERS];
    GtkWidget  *load_rows[UPERF_MAX_CPUS];

    /* Scheduler card */
    GtkWidget  *row_active_pid;
    GtkWidget  *row_tracked;

    /* Mode selector buttons (linked group) */
    GtkWidget  *mode_buttons[4];

    /* Games */
    GtkWidget  *games_group;
    GtkWidget  *games_placeholder;
    GtkWidget **game_rows;
    int         game_rows_len;

    /* Logs */
    GtkTextView *log_view;

    /* Frequency override */
    GtkWidget    *freq_toggle;
    GtkAdjustment *freq_adj[4];
    GtkWidget    *freq_scale_rows[4];

    /* Toasts */
    AdwToastOverlay *toasts;
} AppState;

static AppState g_app;

static void refresh_games(void);

/* ---------------------------------------------------------------- */

static void toast(const char *text) {
    if (g_app.toasts)
        adw_toast_overlay_add_toast(g_app.toasts, adw_toast_new(text));
}

static const char *mode_display_name(const char *mode) {
    if (!mode) return "Unknown";
    if (!g_strcmp0(mode, "balance"))     return "Balance";
    if (!g_strcmp0(mode, "powersave"))   return "Power Save";
    if (!g_strcmp0(mode, "performance")) return "Performance";
    if (!g_strcmp0(mode, "fast"))        return "Fast";
    return mode;
}

/* ----------------------------------------------------------------
 * Live refresh: pull cached proxy state into the widgets.
 * ---------------------------------------------------------------- */

static void refresh_display(void) {
    if (!g_app.proxy) return;
    DbusProxy *p = g_app.proxy;

    if (g_app.row_mode)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.row_mode),
                                    mode_display_name(p->current_mode));

    /* Keep the mode selector in sync with the daemon's actual mode. */
    static const char *modes[] = { "balance", "powersave", "performance", "fast" };
    for (int i = 0; i < 4; i++) {
        if (!g_app.mode_buttons[i]) continue;
        gboolean active = !g_strcmp0(p->current_mode, modes[i]);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_app.mode_buttons[i])) != active)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_app.mode_buttons[i]), active);
    }

    if (g_app.row_scene)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.row_scene),
                                    p->current_scene ? p->current_scene : "—");

    if (g_app.row_heavy) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.row_heavy),
            p->is_heavy_load ? "Heavy load active" : "Normal");
        /* Recolor the row to signal state without a jumping label. */
        if (p->is_heavy_load)
            gtk_widget_add_css_class(g_app.row_heavy, "warning");
        else
            gtk_widget_remove_css_class(g_app.row_heavy, "warning");
    }

    if (g_app.row_temp) {
        char buf[64];
        g_snprintf(buf, sizeof(buf), "%.1f °C", p->max_temp / 1000.0);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.row_temp), buf);
    }
    if (g_app.temp_bar) {
        double frac = p->max_temp > 0
            ? CLAMP((p->max_temp / 1000.0 - 40.0) / 60.0, 0.0, 1.0) : 0.0;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_app.temp_bar), frac);
    }
    if (g_app.row_thermal)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.row_thermal),
                                    p->thermal_state ? p->thermal_state : "—");

    if (g_app.row_active_pid) {
        char buf[32];
        if (p->active_pid > 0)
            g_snprintf(buf, sizeof(buf), "%d", p->active_pid);
        else
            g_strlcpy(buf, "None", sizeof(buf));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.row_active_pid), buf);
    }
    if (g_app.row_tracked) {
        char buf[48];
        g_snprintf(buf, sizeof(buf), "%d processes · %d threads",
                   p->tracked_processes, p->tracked_threads);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.row_tracked), buf);
    }

    for (int i = 0; i < p->nr_freqs && i < UPERF_MAX_CLUSTERS; i++) {
        if (!g_app.freq_rows[i]) continue;
        char buf[32];
        g_snprintf(buf, sizeof(buf), "%.2f GHz", p->freqs[i] / 1000.0);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.freq_rows[i]), buf);
    }
    for (int i = 0; i < p->nr_loads && i < UPERF_MAX_CPUS; i++) {
        if (!g_app.load_rows[i]) continue;
        char buf[32];
        g_snprintf(buf, sizeof(buf), "%.0f %%", p->loads[i]);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.load_rows[i]), buf);
    }
}

/* Signal bridges */
static void on_mode_changed(DbusProxy *p, gchar *m, gpointer ud)
{ (void)p; (void)m; (void)ud; refresh_display(); }
static void on_scene_changed(DbusProxy *p, gchar *m, gpointer ud)
{ (void)p; (void)m; (void)ud; refresh_display(); }
static void on_stats_updated(DbusProxy *p, gpointer ud)
{ (void)p; (void)ud; refresh_display(); refresh_games(); }
static void on_heavy_changed(DbusProxy *p, gboolean h, gpointer ud)
{ (void)p; (void)h; (void)ud; refresh_display(); }
static void on_thermal_changed(DbusProxy *p, gint32 t, gpointer ud)
{ (void)p; (void)t; (void)ud; refresh_display(); }

/* Mode selector: a linked toggle group. Only react to user activation,
 * not to programmatic sync in refresh_display(). */
static void on_mode_toggled(GtkToggleButton *btn, gpointer ud) {
    if (!gtk_toggle_button_get_active(btn)) return;
    const char *mode = ud;
    if (g_app.proxy && !dbus_proxy_set_mode(g_app.proxy, mode))
        toast("Failed to set power mode");
}

/* Games: per-app mode dropdown */
typedef struct { gint pid; gchar *app; } GameTarget;

static void game_target_free(gpointer data, GClosure *closure) {
    (void)closure;
    GameTarget *t = data;
    if (!t) return;
    g_free(t->app);
    g_free(t);
}

static void on_game_mode_selected(GObject *obj, GParamSpec *pspec, gpointer ud) {
    (void)pspec;
    if (!g_app.proxy) return;
    GameTarget *t = ud;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    static const char *modes[] = { "balance", "powersave", "performance", "fast" };
    if (t && idx < G_N_ELEMENTS(modes))
        dbus_proxy_set_game_mode(g_app.proxy, t->pid, t->app, modes[idx]);
}

/* Logs */
static void on_refresh_logs(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(g_app.log_view);
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &err,
        "journalctl", "-u", "uperf-linux.service", "-n", "200", "--no-pager", NULL);
    if (!proc) {
        gtk_text_buffer_set_text(buf, err ? err->message : "Unable to start journalctl", -1);
        g_clear_error(&err);
        return;
    }
    gchar *out = NULL;
    if (!g_subprocess_communicate_utf8(proc, NULL, NULL, &out, NULL, &err)) {
        gtk_text_buffer_set_text(buf, err ? err->message : "Unable to read journal", -1);
        g_clear_error(&err);
    } else {
        gtk_text_buffer_set_text(buf, out && *out ? out : "(journal is empty)", -1);
    }
    g_free(out);
    g_object_unref(proc);
}

static void on_clear_logs(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(g_app.log_view), "", -1);
}

/* Settings */
static void on_reload_config(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    if (g_app.proxy && dbus_proxy_reload_config(g_app.proxy))
        toast("Configuration reloaded");
    else
        toast("Reload failed");
}

/* Frequency override */
static void on_freq_toggle(GObject *obj, GParamSpec *pspec, gpointer ud) {
    (void)pspec; (void)ud;
    gboolean on = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
    for (int i = 0; i < 4; i++)
        if (g_app.freq_scale_rows[i])
            gtk_widget_set_sensitive(g_app.freq_scale_rows[i], on);
}

static void on_apply_freq(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    if (!g_app.proxy || !g_app.freq_toggle) return;
    if (!adw_switch_row_get_active(ADW_SWITCH_ROW(g_app.freq_toggle))) {
        dbus_proxy_release_freq_override(g_app.proxy);
        toast("Override released");
        return;
    }
    /* CPU sliders are in kHz (daemon API wants Hz); GPU slider is already Hz. */
    gint64 prime = (gint64)gtk_adjustment_get_value(g_app.freq_adj[0]) * 1000;
    gint64 perf  = (gint64)gtk_adjustment_get_value(g_app.freq_adj[1]) * 1000;
    gint64 eff   = (gint64)gtk_adjustment_get_value(g_app.freq_adj[2]) * 1000;
    gint64 gpu   = (gint64)gtk_adjustment_get_value(g_app.freq_adj[3]);
    if (dbus_proxy_apply_freq_override(g_app.proxy, prime, perf, eff, gpu))
        toast("Frequency override applied");
    else {
        adw_switch_row_set_active(ADW_SWITCH_ROW(g_app.freq_toggle), FALSE);
        toast("Failed to apply override");
    }
}

static void on_release_freq(GtkButton *btn, gpointer ud) {
    (void)btn; (void)ud;
    if (g_app.proxy) dbus_proxy_release_freq_override(g_app.proxy);
    if (g_app.freq_toggle)
        adw_switch_row_set_active(ADW_SWITCH_ROW(g_app.freq_toggle), FALSE);
    toast("Override released");
}

/* ----------------------------------------------------------------
 * Page scaffolding helper: a scrollable AdwPreferencesPage.
 * ---------------------------------------------------------------- */

static GtkWidget *new_prefs_page(const char *title, const char *icon) {
    GtkWidget *page = adw_preferences_page_new();
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), title);
    if (icon)
        adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page), icon);
    return page;
}

static GtkWidget *value_row(GtkWidget *group, const char *title) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "—");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    return row;
}

/* ----------------------------------------------------------------
 * Dashboard
 * ---------------------------------------------------------------- */

static GtkWidget *create_dashboard_page(void) {
    GtkWidget *page = new_prefs_page("Dashboard", "speedometer-symbolic");

    /* --- Power mode selector (linked toggle group) --- */
    GtkWidget *mode_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(mode_group), "Power Mode");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(mode_group));

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(btn_box, "linked");
    gtk_widget_set_margin_top(btn_box, 4);
    gtk_widget_set_margin_bottom(btn_box, 4);

    static const char *modes[]   = { "balance", "powersave", "performance", "fast" };
    static const char *labels[]  = { "Balance", "Power Save", "Performance", "Fast" };
    GtkWidget *first = NULL;
    for (int i = 0; i < 4; i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label(labels[i]);
        gtk_widget_set_hexpand(b, TRUE);
        if (first)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(first));
        else
            first = b;
        g_signal_connect(b, "toggled", G_CALLBACK(on_mode_toggled), (gpointer)modes[i]);
        g_app.mode_buttons[i] = b;
        gtk_box_append(GTK_BOX(btn_box), b);
    }
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(mode_group), btn_box);

    /* --- Status card --- */
    GtkWidget *status = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(status), "Status");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(status));
    g_app.row_mode  = value_row(status, "Active Mode");
    g_app.row_scene = value_row(status, "Scene");
    g_app.row_heavy = value_row(status, "Load State");

    /* --- Scheduler card (affinity / cgroup activity) --- */
    GtkWidget *sched = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(sched), "Scheduler");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(sched),
        "Thread affinity and cgroup management activity.");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(sched));
    g_app.row_active_pid = value_row(sched, "Active Foreground PID");
    g_app.row_tracked    = value_row(sched, "Managed Processes / Threads");

    /* --- Per-cluster frequency card --- */
    GtkWidget *freq = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(freq), "Cluster Frequency");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(freq));
    for (int i = 0; i < 3; i++)
        g_app.freq_rows[i] = value_row(freq, CLUSTER_NAMES[i]);

    /* --- Per-CPU utilization card --- */
    GtkWidget *load = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(load), "CPU Utilization");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(load));
    for (int i = 0; i < UPERF_MAX_CPUS; i++) {
        char name[16];
        g_snprintf(name, sizeof(name), "CPU %d", i);
        g_app.load_rows[i] = value_row(load, name);
    }

    /* --- Thermal card (with a real progress bar) --- */
    GtkWidget *therm = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(therm), "Thermal");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(therm));
    g_app.row_temp    = value_row(therm, "Max Temperature");
    g_app.row_thermal = value_row(therm, "Thermal State");

    g_app.temp_bar = gtk_progress_bar_new();
    gtk_widget_set_hexpand(g_app.temp_bar, TRUE);
    gtk_widget_set_margin_top(g_app.temp_bar, 6);
    gtk_widget_set_margin_bottom(g_app.temp_bar, 6);
    gtk_widget_set_margin_start(g_app.temp_bar, 6);
    gtk_widget_set_margin_end(g_app.temp_bar, 6);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(therm), g_app.temp_bar);

    return page;
}

/* ----------------------------------------------------------------
 * Games
 * ---------------------------------------------------------------- */

static void refresh_games(void) {
    if (!g_app.games_group || !g_app.proxy) return;

    /* Remove previously-added rows. */
    for (int i = 0; i < g_app.game_rows_len; i++) {
        if (g_app.game_rows[i])
            adw_preferences_group_remove(ADW_PREFERENCES_GROUP(g_app.games_group),
                                         g_app.game_rows[i]);
    }
    g_free(g_app.game_rows);
    g_app.game_rows = NULL;
    g_app.game_rows_len = 0;

    int n = g_app.proxy->nr_games;
    gtk_widget_set_visible(g_app.games_placeholder, n == 0);
    if (n == 0) return;

    g_app.game_rows = g_malloc0(n * sizeof(GtkWidget *));
    g_app.game_rows_len = n;

    static const char *choices[] = { "balance", "powersave", "performance", "fast", NULL };

    for (int i = 0; i < n; i++) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      g_app.proxy->game_comm[i]);
        /* Look up the cgroup class the scheduler assigned to this PID. */
        const char *cls = NULL;
        for (int w = 0; w < g_app.proxy->nr_workloads; w++) {
            if (g_app.proxy->wl_pid[w] == g_app.proxy->game_pid[i]) {
                cls = g_app.proxy->wl_class[w];
                break;
            }
        }
        char sub[96];
        if (cls && *cls && g_strcmp0(cls, "—") != 0)
            g_snprintf(sub, sizeof(sub), "PID %d · class: %s",
                       g_app.proxy->game_pid[i], cls);
        else
            g_snprintf(sub, sizeof(sub), "PID %d · unmanaged",
                       g_app.proxy->game_pid[i]);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row),
            gtk_image_new_from_icon_name("applications-games-symbolic"));

        GtkStringList *list = gtk_string_list_new(choices);
        GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
        gtk_widget_set_valign(dd, GTK_ALIGN_CENTER);
        g_object_unref(list);

        const gchar *cur = g_app.proxy->game_mode[i];
        for (guint j = 0; choices[j]; j++)
            if (!g_strcmp0(choices[j], cur)) {
                gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), j);
                break;
            }

        GameTarget *t = g_new0(GameTarget, 1);
        t->pid = g_app.proxy->game_pid[i];
        t->app = g_strdup(g_app.proxy->game_comm[i]);
        g_signal_connect_data(dd, "notify::selected",
                              G_CALLBACK(on_game_mode_selected), t,
                              game_target_free, 0);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), dd);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_app.games_group), row);
        g_app.game_rows[i] = row;
    }
}

static GtkWidget *create_games_page(void) {
    GtkWidget *page = new_prefs_page("Games", "applications-games-symbolic");

    g_app.games_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g_app.games_group),
                                    "Detected Games");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(g_app.games_group),
        "Running game and game-like processes. Assign a per-app power mode.");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(g_app.games_group));

    /* Placeholder shown when nothing is detected. */
    g_app.games_placeholder = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_app.games_placeholder),
                                  "No games detected");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(g_app.games_placeholder),
                                "Launch a game and it will appear here.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_app.games_group),
                              g_app.games_placeholder);

    return page;
}

/* ----------------------------------------------------------------
 * Frequency override
 * ---------------------------------------------------------------- */

static GtkWidget *create_frequency_page(void) {
    GtkWidget *page = new_prefs_page("Frequency", "power-profile-performance-symbolic");

    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Manual Frequency Override");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
        "Lock each cluster to a fixed frequency. Disable to return to "
        "automatic scaling.");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    /* Enable toggle */
    g_app.freq_toggle = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_app.freq_toggle),
                                  "Override Enabled");
    g_signal_connect(g_app.freq_toggle, "notify::active",
                     G_CALLBACK(on_freq_toggle), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_app.freq_toggle);

    struct { const char *title; const char *unit; gdouble min, max, def; } cl[] = {
        { "Prime",       "kHz", 595200,    2956800,   2956800   },
        { "Performance", "kHz", 499200,    2803200,   2803200   },
        { "Efficiency",  "kHz", 307200,    2016000,   2016000   },
        { "GPU",         "Hz",  220000000, 680000000, 680000000 },
    };

    for (int i = 0; i < 4; i++) {
        /* AdwActionRow holds the label; a scale sits in the row body below. */
        GtkWidget *row = adw_action_row_new();
        char title[64];
        g_snprintf(title, sizeof(title), "%s (%s)", cl[i].title, cl[i].unit);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

        g_app.freq_adj[i] = gtk_adjustment_new(cl[i].def, cl[i].min, cl[i].max,
                                               50000, 100000, 0);
        GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, g_app.freq_adj[i]);
        gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
        gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
        gtk_widget_set_hexpand(scale, TRUE);
        gtk_widget_set_size_request(scale, 260, -1);
        gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), scale);

        gtk_widget_set_sensitive(row, FALSE);  /* until override enabled */
        g_app.freq_scale_rows[i] = row;
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    /* Action buttons */
    GtkWidget *btns = adw_preferences_group_new();
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(btns));
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *apply = gtk_button_new_with_label("Apply");
    gtk_widget_add_css_class(apply, "suggested-action");
    gtk_widget_add_css_class(apply, "pill");
    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply_freq), NULL);
    gtk_box_append(GTK_BOX(hbox), apply);

    GtkWidget *release = gtk_button_new_with_label("Release All");
    gtk_widget_add_css_class(release, "pill");
    g_signal_connect(release, "clicked", G_CALLBACK(on_release_freq), NULL);
    gtk_box_append(GTK_BOX(hbox), release);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(btns), hbox);
    return page;
}

/* ----------------------------------------------------------------
 * Settings
 * ---------------------------------------------------------------- */

static GtkWidget *create_settings_page(void) {
    GtkWidget *page = new_prefs_page("Settings", "emblem-system-symbolic");

    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Daemon Configuration");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  "/etc/uperf-linux/config.json");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
        "Edit the JSON with administrator privileges, then reload it here.");
    GtkWidget *reload = gtk_button_new_with_label("Reload");
    gtk_widget_set_valign(reload, GTK_ALIGN_CENTER);
    g_signal_connect(reload, "clicked", G_CALLBACK(on_reload_config), NULL);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), reload);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

    return page;
}

/* ----------------------------------------------------------------
 * Logs
 * ---------------------------------------------------------------- */

static GtkWidget *create_logs_page(void) {
    GtkWidget *page = new_prefs_page("Logs", "text-x-generic-symbolic");

    GtkWidget *group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Service Journal");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    GtkWidget *buf_holder = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(buf_holder),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(buf_holder, -1, 320);
    gtk_widget_add_css_class(buf_holder, "card");

    GtkTextBuffer *buf = gtk_text_buffer_new(NULL);
    gtk_text_buffer_set_text(buf,
        "Press Refresh to load the latest uperf-linux.service journal.\n", -1);
    g_app.log_view = GTK_TEXT_VIEW(gtk_text_view_new_with_buffer(buf));
    gtk_text_view_set_editable(g_app.log_view, FALSE);
    gtk_text_view_set_monospace(g_app.log_view, TRUE);
    gtk_text_view_set_wrap_mode(g_app.log_view, GTK_WRAP_WORD_CHAR);
    gtk_widget_set_margin_top(GTK_WIDGET(g_app.log_view), 6);
    gtk_widget_set_margin_bottom(GTK_WIDGET(g_app.log_view), 6);
    gtk_widget_set_margin_start(GTK_WIDGET(g_app.log_view), 6);
    gtk_widget_set_margin_end(GTK_WIDGET(g_app.log_view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(buf_holder),
                                  GTK_WIDGET(g_app.log_view));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), buf_holder);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(hbox, 8);
    GtkWidget *refresh = gtk_button_new_with_label("Refresh");
    gtk_widget_add_css_class(refresh, "pill");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_logs), NULL);
    gtk_box_append(GTK_BOX(hbox), refresh);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(clear, "pill");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_logs), NULL);
    gtk_box_append(GTK_BOX(hbox), clear);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), hbox);

    return page;
}

/* ----------------------------------------------------------------
 * Window assembly
 * ---------------------------------------------------------------- */

static void on_activate(GtkApplication *app, gpointer ud) {
    (void)ud;

    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "uperf-linux");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 720);

    /* Responsive view stack + switcher (title-bar on desktop, bottom bar
     * on narrow displays via AdwViewSwitcherBar). */
    GtkWidget *view_stack = adw_view_stack_new();

    struct { GtkWidget *(*build)(void); const char *name, *title, *icon; } pages[] = {
        { create_dashboard_page, "dashboard", "Dashboard", "speedometer-symbolic" },
        { create_games_page,     "games",     "Games",     "applications-games-symbolic" },
        { create_frequency_page, "frequency", "Frequency", "power-profile-performance-symbolic" },
        { create_settings_page,  "settings",  "Settings",  "emblem-system-symbolic" },
        { create_logs_page,      "logs",      "Logs",      "text-x-generic-symbolic" },
    };
    for (int i = 0; i < 5; i++) {
        AdwViewStackPage *sp = adw_view_stack_add_titled(
            ADW_VIEW_STACK(view_stack), pages[i].build(),
            pages[i].name, pages[i].title);
        adw_view_stack_page_set_icon_name(sp, pages[i].icon);
    }

    /* Header carries a wide view switcher; on narrow widths a breakpoint
     * hides it and reveals the bottom switcher bar instead. */
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher),
                                ADW_VIEW_STACK(view_stack));
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher),
                                 ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);

    GtkWidget *switcher_bar = adw_view_switcher_bar_new();
    adw_view_switcher_bar_set_stack(ADW_VIEW_SWITCHER_BAR(switcher_bar),
                                    ADW_VIEW_STACK(view_stack));

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), switcher_bar);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), view_stack);

    g_app.toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(g_app.toasts, toolbar);

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window),
                                       GTK_WIDGET(g_app.toasts));

    /* Narrow layout: hide the header switcher, reveal the bottom bar. */
    AdwBreakpoint *bp = adw_breakpoint_new(
        adw_breakpoint_condition_parse("max-width: 500px"));
    GValue v_false = G_VALUE_INIT;
    g_value_init(&v_false, G_TYPE_BOOLEAN);
    g_value_set_boolean(&v_false, FALSE);
    GValue v_true = G_VALUE_INIT;
    g_value_init(&v_true, G_TYPE_BOOLEAN);
    g_value_set_boolean(&v_true, TRUE);
    adw_breakpoint_add_setter(bp, G_OBJECT(switcher), "visible", &v_false);
    adw_breakpoint_add_setter(bp, G_OBJECT(switcher_bar), "reveal", &v_true);
    g_value_unset(&v_false);
    g_value_unset(&v_true);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), bp);

    refresh_display();
    refresh_games();
    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    adw_init();

    DbusProxy *proxy = g_object_new(UPERF_TYPE_DBUS_PROXY, NULL);
    g_app.proxy = proxy;
    dbus_proxy_start_polling(proxy);

    dbus_proxy_set_mode_cb(proxy,    G_CALLBACK(on_mode_changed),    NULL);
    dbus_proxy_set_scene_cb(proxy,   G_CALLBACK(on_scene_changed),   NULL);
    dbus_proxy_set_stats_cb(proxy,   G_CALLBACK(on_stats_updated),   NULL);
    dbus_proxy_set_heavy_cb(proxy,   G_CALLBACK(on_heavy_changed),   NULL);
    dbus_proxy_set_thermal_cb(proxy, G_CALLBACK(on_thermal_changed), NULL);

    AdwApplication *app = adw_application_new("org.uperflinux.gui",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_clear_object(&app);
    g_clear_object(&proxy);
    g_clear_pointer(&g_app.game_rows, g_free);
    return status;
}
