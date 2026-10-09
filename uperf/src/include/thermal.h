#ifndef UPERF_THERMAL_H
#define UPERF_THERMAL_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum number of thermal zones we monitor */
#define THERMAL_MAX_ZONES 64

/* Maximum length of a thermal zone name */
#define THERMAL_NAME_LEN 64

/* Thermal zone type constants */
typedef enum {
    THERMAL_ZONE_CPU = 0,
    THERMAL_ZONE_GPU,
    THERMAL_ZONE_SOC,
    THERMAL_ZONE_BATTERY,
    THERMAL_ZONE_BOARD,
    THERMAL_ZONE_UNKNOWN
} ThermalZoneType;

/* A single thermal zone */
typedef struct {
    int id;                      /* thermal_zoneN */
    char name[THERMAL_NAME_LEN]; /* e.g., "cpu-thermal" */
    char type[THERMAL_NAME_LEN]; /* e.g., "cpu-thermal" */
    int  temp_millidegC;         /* current temperature in millidegrees C */
    ThermalZoneType zone_type;
    bool valid;                  /* true if temperature read succeeded */
} ThermalZone;

/* Thermal policy thresholds (configurable) */
typedef struct {
    /* Temperature limits in millidegrees C (e.g., 70000 = 70°C) */
    int warn_temp;       /* Warning threshold — log and optionally reduce */
    int throttle_temp;   /* Throttle threshold — reduce CPU/GPU frequency */
    int critical_temp;   /* Critical threshold — emergency shutdown */
    int recovery_temp;   /* Recovery threshold — restore normal operation */
} ThermalPolicy;

/* Thermal state */
typedef enum {
    THERMAL_NORMAL = 0,     /* All zones below warn_temp */
    THERMAL_WARNING,        /* One or more zones above warn_temp */
    THERMAL_THROTTLED,      /* One or more zones above throttle_temp */
    THERMAL_CRITICAL,       /* One or more zones above critical_temp */
    THERMAL_NUM_STATES
} ThermalState;

/* Opaque thermal manager handle */
typedef struct ThermalManager ThermalManager;

/* Create a new thermal manager.
 * policy: thermal thresholds (use thermal_default_policy() for sensible defaults).
 * Returns NULL on failure. */
ThermalManager *thermal_manager_new(const ThermalPolicy *policy);

/* Free thermal manager resources. */
void thermal_manager_free(ThermalManager *tm);

/* Discover all thermal zones on the system.
 * Scans /sys/class/thermal/thermal_zoneN/ for available zones.
 * Returns number of zones found, or -1 on error. */
int thermal_manager_discover_zones(ThermalManager *tm);

/* Read temperatures from all discovered zones.
 * Returns the current ThermalState. */
ThermalState thermal_manager_update(ThermalManager *tm);

/* Pure hysteresis helper used by the manager and unit tests. */
ThermalState thermal_policy_next_state(const ThermalPolicy *policy,
                                       ThermalState current,
                                       int max_temp_millidegC);

/* Require a candidate state to remain stable before applying it. Critical
 * escalation is immediate; other transitions use time-based dwell periods. */
ThermalState thermal_debounce_transition(ThermalState current,
                                         ThermalState proposed,
                                         ThermalState *pending,
                                         int64_t *pending_since_ms,
                                         int64_t now_ms);

/* Get the number of discovered zones. */
int thermal_manager_zone_count(const ThermalManager *tm);

/* Get a specific thermal zone by index. */
const ThermalZone *thermal_manager_get_zone(const ThermalManager *tm, int index);

/* Get the highest temperature across all zones (in millidegrees C). */
int thermal_manager_get_max_temp(const ThermalManager *tm);

/* Get the current thermal state. */
ThermalState thermal_manager_get_state(const ThermalManager *tm);

/* Get the recommended frequency reduction factor based on current thermal state.
 * Returns a value between 0.0 (full throttle) and 1.0 (reduce to minimum).
 * 0.0 = no reduction needed, 1.0 = maximum reduction. */
float thermal_manager_get_reduction_factor(const ThermalManager *tm);

/* Check if a specific zone type exists (e.g., "gpu"). */
bool thermal_manager_has_zone_type(const ThermalManager *tm, ThermalZoneType type);

/* Get the zone of a specific type, or -1 if not found. */
int thermal_manager_find_zone_by_type(const ThermalManager *tm, ThermalZoneType type);

/* Get human-readable string for thermal state. */
const char *thermal_state_to_string(ThermalState state);

/* Get default thermal policy for SM8550 (Snapdragon 8 Gen 2). */
ThermalPolicy thermal_default_policy(void);

/* Get default thermal policy for general use (conservative). */
ThermalPolicy thermal_conservative_policy(void);

#endif /* UPERF_THERMAL_H */
