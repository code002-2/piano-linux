#define _GNU_SOURCE
#include "config.h"

#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DAEMON_BUS   "org.uperflinux.Daemon"
#define DAEMON_PATH  "/org/uperflinux/Daemon"
#define DAEMON_IFACE "org.uperflinux.Daemon"
#define CONTROL_CALL_TIMEOUT_MS 120000

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <command> [args]\n\n"
        "Commands:\n"
        "  status                         Show daemon and scheduler status\n"
        "  mode <name>                    Set balance|powersave|performance\n"
        "  game-list                      List detected game processes\n"
        "  active-pid <pid|0>             Select/clear the active workload\n"
        "  set-freq <cluster> <freq_hz>   Set/release a manual override\n"
        "      cluster: -1=GPU, 0=Prime, 1=Perf, 2=Eff, 3=all CPUs\n"
        "      freq_hz: 0 releases the override\n"
        "  show-freqs                     Show frequencies reported by daemon\n"
        "  detect                         Run the hardware config wizard\n"
        "  help                           Show this help\n",
        prog);
}

static GDBusConnection *connect_daemon(void) {
    GError *error = NULL;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL,
                                                  &error);
    if (!connection) {
        fprintf(stderr, "Cannot connect to system D-Bus: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    return connection;
}

static GVariant *daemon_call(GDBusConnection *connection,
                             const char *method,
                             GVariant *parameters,
                             const GVariantType *reply_type) {
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        connection, DAEMON_BUS, DAEMON_PATH, DAEMON_IFACE, method,
        parameters, reply_type, G_DBUS_CALL_FLAGS_NONE,
        CONTROL_CALL_TIMEOUT_MS, NULL, &error);
    if (!reply) {
        fprintf(stderr, "Daemon call %s failed: %s\n", method,
                error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    return reply;
}

static GVariant *get_all_properties(GDBusConnection *connection) {
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        connection, DAEMON_BUS, DAEMON_PATH,
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", DAEMON_IFACE), G_VARIANT_TYPE("(a{sv})"),
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply) {
        fprintf(stderr, "uperf-linux daemon is unavailable: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return NULL;
    }
    GVariant *properties = NULL;
    g_variant_get(reply, "(@a{sv})", &properties);
    g_variant_unref(reply);
    return properties;
}

static void print_frequencies(GVariant *properties) {
    GVariant *frequencies = g_variant_lookup_value(
        properties, "CpuFrequencies", G_VARIANT_TYPE("ad"));
    printf("CPU frequencies:\n");
    if (!frequencies || g_variant_n_children(frequencies) == 0) {
        printf("  (no samples yet)\n");
    } else {
        gsize count = 0;
        const gdouble *values = g_variant_get_fixed_array(
            frequencies, &count, sizeof(*values));
        for (gsize i = 0; i < count; i++)
            printf("  CPU%zu: %.0f MHz\n", i, values[i]);
    }
    if (frequencies) g_variant_unref(frequencies);
}

static int cmd_status(void) {
    GDBusConnection *connection = connect_daemon();
    if (!connection) return 1;
    GVariant *properties = get_all_properties(connection);
    g_object_unref(connection);
    if (!properties) return 1;

    const char *mode = "unknown";
    const char *scene = "unknown";
    gboolean heavy = FALSE;
    gint32 temperature = 0;
    gint32 active_pid = 0;
    g_variant_lookup(properties, "CurrentMode", "&s", &mode);
    g_variant_lookup(properties, "CurrentScene", "&s", &scene);
    g_variant_lookup(properties, "IsHeavyLoad", "b", &heavy);
    g_variant_lookup(properties, "MaxTemperature", "i", &temperature);
    g_variant_lookup(properties, "ActiveProcess", "i", &active_pid);
    printf("Power mode: %s\nScene: %s\nHeavy load: %s\nActive PID: %d\n",
           mode, scene, heavy ? "yes" : "no", active_pid);
    if (temperature > 0)
        printf("Max temperature: %.1f C\n", temperature / 1000.0);
    printf("\n");
    print_frequencies(properties);

    GVariant *games = g_variant_lookup_value(
        properties, "GameProcesses", G_VARIANT_TYPE("a(issss)"));
    printf("\nDetected game processes: %zu\n",
           games ? g_variant_n_children(games) : 0);
    if (games) g_variant_unref(games);
    g_variant_unref(properties);
    return 0;
}

static int cmd_mode(const char *mode) {
    if (strcmp(mode, "balance") != 0 && strcmp(mode, "powersave") != 0 &&
        strcmp(mode, "performance") != 0) {
        fprintf(stderr, "Invalid mode: %s\n", mode);
        return 1;
    }
    GDBusConnection *connection = connect_daemon();
    if (!connection) return 1;
    GVariant *reply = daemon_call(connection, "SetMode",
                                  g_variant_new("(s)", mode),
                                  G_VARIANT_TYPE("(b)"));
    g_object_unref(connection);
    if (!reply) return 1;
    gboolean success = FALSE;
    g_variant_get(reply, "(b)", &success);
    g_variant_unref(reply);
    if (!success) {
        fprintf(stderr, "Daemon rejected mode '%s'\n", mode);
        return 1;
    }
    printf("Power mode set to: %s\n", mode);
    return 0;
}

static int cmd_game_list(void) {
    GDBusConnection *connection = connect_daemon();
    if (!connection) return 1;
    GVariant *properties = get_all_properties(connection);
    g_object_unref(connection);
    if (!properties) return 1;
    GVariant *games = g_variant_lookup_value(
        properties, "GameProcesses", G_VARIANT_TYPE("a(issss)"));
    g_variant_unref(properties);
    if (!games) return 1;

    gsize count = g_variant_n_children(games);
    for (gsize i = 0; i < count; i++) {
        gint pid;
        const gchar *comm, *cmdline, *package, *mode;
        g_variant_get_child(games, i, "(i&s&s&s&s)", &pid, &comm,
                            &cmdline, &package, &mode);
        (void)cmdline;
        (void)package;
        printf("  PID %-6d %-16s %s\n", pid, comm, mode);
    }
    if (count == 0) printf("  (no game processes detected)\n");
    printf("Total: %zu game process(es)\n", count);
    g_variant_unref(games);
    return 0;
}

static int cmd_set_freq(const char *cluster_text, const char *freq_text) {
    char *end = NULL;
    errno = 0;
    long cluster = strtol(cluster_text, &end, 10);
    if (errno || end == cluster_text || *end || cluster < -1 || cluster > 3) {
        fprintf(stderr, "Invalid cluster: %s\n", cluster_text);
        return 1;
    }
    end = NULL;
    errno = 0;
    gint64 frequency = g_ascii_strtoll(freq_text, &end, 10);
    if (errno || end == freq_text || *end || frequency < 0) {
        fprintf(stderr, "Invalid frequency: %s\n", freq_text);
        return 1;
    }

    GDBusConnection *connection = connect_daemon();
    if (!connection) return 1;
    GVariant *reply = daemon_call(connection, "SetManualFreq",
                                  g_variant_new("(ix)", (gint)cluster,
                                                frequency),
                                  G_VARIANT_TYPE("(b)"));
    g_object_unref(connection);
    if (!reply) return 1;
    gboolean success = FALSE;
    g_variant_get(reply, "(b)", &success);
    g_variant_unref(reply);
    if (!success) {
        fprintf(stderr, "Daemon rejected the frequency (check hardware range and permissions)\n");
        return 1;
    }
    printf("Manual override %s for cluster %ld\n",
           frequency == 0 ? "released" : "applied", cluster);
    return 0;
}

static int cmd_active_pid(const char *pid_text) {
    char *end = NULL;
    errno = 0;
    long pid = strtol(pid_text, &end, 10);
    if (errno || end == pid_text || *end || pid < 0 || pid > G_MAXINT32) {
        fprintf(stderr, "Invalid PID: %s\n", pid_text);
        return 1;
    }
    GDBusConnection *connection = connect_daemon();
    if (!connection) return 1;
    GVariant *reply = daemon_call(connection, "SetActiveProcess",
                                  g_variant_new("(i)", (gint)pid),
                                  G_VARIANT_TYPE("(b)"));
    g_object_unref(connection);
    if (!reply) return 1;
    gboolean success = FALSE;
    g_variant_get(reply, "(b)", &success);
    g_variant_unref(reply);
    if (!success) {
        fprintf(stderr, "Daemon rejected active PID %ld\n", pid);
        return 1;
    }
    if (pid == 0)
        printf("Active workload selection cleared\n");
    else
        printf("Active workload set to PID %ld\n", pid);
    return 0;
}

static int cmd_show_freqs(void) {
    GDBusConnection *connection = connect_daemon();
    if (!connection) return 1;
    GVariant *properties = get_all_properties(connection);
    g_object_unref(connection);
    if (!properties) return 1;
    print_frequencies(properties);
    g_variant_unref(properties);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    const char *command = argv[1];
    if (strcmp(command, "help") == 0 || strcmp(command, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (strcmp(command, "version") == 0 || strcmp(command, "--version") == 0) {
        puts("uperfctl v0.1.0");
        return 0;
    }
    if (strcmp(command, "status") == 0) return cmd_status();
    if (strcmp(command, "game-list") == 0) return cmd_game_list();
    if (strcmp(command, "show-freqs") == 0) return cmd_show_freqs();
    if (strcmp(command, "mode") == 0)
        return argc == 3 ? cmd_mode(argv[2]) : (print_usage(argv[0]), 1);
    if (strcmp(command, "set-freq") == 0)
        return argc == 4 ? cmd_set_freq(argv[2], argv[3])
                         : (print_usage(argv[0]), 1);
    if (strcmp(command, "active-pid") == 0)
        return argc == 3 ? cmd_active_pid(argv[2])
                         : (print_usage(argv[0]), 1);
    if (strcmp(command, "detect") == 0) {
        execl("/usr/bin/uperf-wizard", "uperf-wizard", "detect",
              (char *)NULL);
        execlp("uperf-wizard", "uperf-wizard", "detect", (char *)NULL);
        fprintf(stderr, "Cannot run uperf-wizard: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "Unknown command: %s\n", command);
    print_usage(argv[0]);
    return 1;
}
