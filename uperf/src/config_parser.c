#include "config.h"
#include "log.h"
#include "state_machine.h"

#include <json-c/json.h>
#include <json-c/json_util.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <unistd.h>

/* ----------------------------------------------------------------
 * JSON parsing helpers (fixed for json-c 0.19 API)
 * ---------------------------------------------------------------- */

static int parse_int_field(struct json_object *obj, const char *field, int *out) {
    struct json_object *jo;
    if (!json_object_object_get_ex(obj, field, &jo))
        return -1;
    *out = (int)json_object_get_int(jo);
    return 0;
}

static int parse_double_field(struct json_object *obj, const char *field, double *out) {
    struct json_object *jo;
    if (!json_object_object_get_ex(obj, field, &jo))
        return -1;
    *out = json_object_get_double(jo);
    return 0;
}

/* Helper: parse a double field into a float variable */
static int parse_float_field(struct json_object *obj, const char *field, float *out) {
    double tmp;
    int rc = parse_double_field(obj, field, &tmp);
    if (rc == 0) *out = (float)tmp;
    return rc;
}

/* Parse a string field safely */
static int parse_string_field(struct json_object *obj, const char *field, const char **out) {
    struct json_object *jo;
    if (!json_object_object_get_ex(obj, field, &jo))
        return -1;
    *out = json_object_get_string(jo);
    return 0;
}

static bool parse_bool_field(struct json_object *obj, const char *field, bool def) {
    struct json_object *jo;
    if (!json_object_object_get_ex(obj, field, &jo))
        return def;
    return json_object_get_boolean(jo);
}

static int parse_cpu_array(struct json_object *array, uint64_t *mask_out,
                           int *cpus_out, int *nr_cpus_out) {
    if (!array || !mask_out ||
        !json_object_is_type(array, json_type_array))
        return -1;
    size_t nr = json_object_array_length(array);
    if (nr == 0 || nr > MAX_CPUS) return -1;

    uint64_t mask = 0;
    for (size_t i = 0; i < nr; i++) {
        struct json_object *value = json_object_array_get_idx(array, i);
        if (!value || !json_object_is_type(value, json_type_int)) return -1;
        int64_t cpu = json_object_get_int64(value);
        if (cpu < 0 || cpu >= MAX_CPUS ||
            (mask & (UINT64_C(1) << cpu)) != 0)
            return -1;
        mask |= UINT64_C(1) << cpu;
        if (cpus_out) cpus_out[i] = (int)cpu;
    }
    *mask_out = mask;
    if (nr_cpus_out) *nr_cpus_out = (int)nr;
    return 0;
}

/* Map SceneState to StatePresets member pointer */
static ActionParams *get_state_action(StatePresets *sp, SceneState scene) {
    switch (scene) {
        case SCENE_IDLE:      return &sp->idle;
        case SCENE_TOUCH:     return &sp->touch;
        case SCENE_TRIGGER:   return &sp->trigger;
        case SCENE_GESTURE:   return &sp->gesture;
        case SCENE_JUNK:      return &sp->junk;
        case SCENE_SWITCH:    return &sp->switch_;
        case SCENE_BOOST:     return &sp->boost;
        default:              return &sp->global;
    }
}

/* ----------------------------------------------------------------
 * Power model parsing
 * ---------------------------------------------------------------- */

static int parse_power_model(struct json_object *mod_obj, Config *cfg) {
    struct json_object *arr;
    if (!json_object_object_get_ex(mod_obj, "powerModel", &arr)) {
        log_error("Missing 'powerModel' array in cpu module");
        return -1;
    }

    int nr = json_object_array_length(arr);
    if (nr <= 0 || nr > MAX_CLUSTERS) {
        log_error("Invalid powerModel array length: %d (expected 1-%d)", nr, MAX_CLUSTERS);
        return -1;
    }

    cfg->cpu.nr_clusters = nr;
    for (int i = 0; i < nr; i++) {
        struct json_object *entry = json_object_array_get_idx(arr, i);
        PowerModelEntry *pm = &cfg->cpu.power_model[i];

        memset(pm, 0, sizeof(*pm));

        if (parse_int_field(entry, "efficiency", &pm->efficiency) < 0) {
            log_error("powerModel[%d]: missing 'efficiency'", i);
            return -1;
        }
        if (parse_int_field(entry, "nr", &pm->nr_cores) < 0) {
            log_error("powerModel[%d]: missing 'nr'", i);
            return -1;
        }
        struct json_object *cpus = NULL;
        if (json_object_object_get_ex(entry, "cpus", &cpus) &&
            parse_cpu_array(cpus, &pm->cpu_mask, NULL, NULL) < 0) {
            log_error("powerModel[%d]: invalid 'cpus' array", i);
            return -1;
        }
        const char *cpumask_name = NULL;
        if (parse_string_field(entry, "cpumask", &cpumask_name) == 0 &&
            cpumask_name) {
            snprintf(pm->cpumask_name, sizeof(pm->cpumask_name), "%s",
                     cpumask_name);
        }
        if (parse_float_field(entry, "typicalPower", &pm->typical_power_w) < 0)
            pm->typical_power_w = 1.0f;
        if (parse_float_field(entry, "typicalFreq", &pm->typical_freq_mhz) < 0)
            pm->typical_freq_mhz = 1000.0f;
        if (parse_float_field(entry, "sweetFreq", &pm->sweet_freq_mhz) < 0)
            pm->sweet_freq_mhz = pm->typical_freq_mhz * 0.7f;
        if (parse_float_field(entry, "plainFreq", &pm->plain_freq_mhz) < 0)
            pm->plain_freq_mhz = pm->typical_freq_mhz * 0.5f;
        if (parse_float_field(entry, "freeFreq", &pm->free_freq_mhz) < 0)
            pm->free_freq_mhz = pm->typical_freq_mhz * 0.25f;

        log_debug("powerModel[%d]: eff=%d nr=%d typP=%.1fW typF=%.0fMHz sweet=%.0f plain=%.0f free=%.0f",
                   i, pm->efficiency, pm->nr_cores,
                   pm->typical_power_w, pm->typical_freq_mhz,
                   pm->sweet_freq_mhz, pm->plain_freq_mhz, pm->free_freq_mhz);
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Preset / ActionParams parsing
 * ---------------------------------------------------------------- */

static int parse_action_params(struct json_object *obj, ActionParams *ap) {
    memset(ap, 0, sizeof(*ap));

    json_object_object_foreach(obj, k, v) {
        if (strncmp(k, "cpu.", 4) != 0)
            continue;
        const char *sub = k + 4;
        double d = json_object_get_double(v);
        if (strcmp(sub, "latencyTime") == 0) {
            ap->latency_time = (float)d;
            ap->tuning_present |= ACTION_TUNE_LATENCY_TIME;
        } else if (strcmp(sub, "slowLimitPower") == 0) {
            ap->slow_limit_power = (float)d;
            ap->tuning_present |= ACTION_TUNE_SLOW_LIMIT_POWER;
        } else if (strcmp(sub, "fastLimitPower") == 0) {
            ap->fast_limit_power = (float)d;
            ap->tuning_present |= ACTION_TUNE_FAST_LIMIT_POWER;
        } else if (strcmp(sub, "fastLimitCapacity") == 0) {
            ap->fast_limit_capacity = (float)d;
            ap->tuning_present |= ACTION_TUNE_FAST_LIMIT_CAPACITY;
        } else if (strcmp(sub, "fastLimitRecoverScale") == 0) {
            ap->fast_limit_recover_scale = (float)d;
            ap->tuning_present |= ACTION_TUNE_FAST_LIMIT_RECOVER_SCALE;
        } else if (strcmp(sub, "margin") == 0) {
            ap->margin = (float)d;
            ap->tuning_present |= ACTION_TUNE_MARGIN;
        } else if (strcmp(sub, "burst") == 0) {
            ap->burst = (float)d;
            ap->tuning_present |= ACTION_TUNE_BURST;
        } else if (strcmp(sub, "guideCap") == 0) {
            ap->guide_cap = json_object_get_boolean(v);
            ap->tuning_present |= ACTION_TUNE_GUIDE_CAP;
        } else if (strcmp(sub, "limitEfficiency") == 0) {
            ap->limit_efficiency = json_object_get_boolean(v);
            ap->tuning_present |= ACTION_TUNE_LIMIT_EFFICIENCY;
        } else if (strcmp(sub, "baseSampleTime") == 0) {
            ap->base_sample_time = (float)d;
            ap->tuning_present |= ACTION_TUNE_BASE_SAMPLE_TIME;
        } else if (strcmp(sub, "baseSlackTime") == 0) {
            ap->base_slack_time = (float)d;
            ap->tuning_present |= ACTION_TUNE_BASE_SLACK_TIME;
        } else if (strcmp(sub, "predictThd") == 0) {
            ap->predict_thd = (float)d;
            ap->tuning_present |= ACTION_TUNE_PREDICT_THD;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Sysfs knobs parsing
 * ---------------------------------------------------------------- */

static int parse_sysfs_knobs(struct json_object *mod_obj, Config *cfg) {
    struct json_object *knobs_obj;
    if (!json_object_object_get_ex(mod_obj, "knob", &knobs_obj)) {
        log_warn("sysfs module: no 'knob' object found");
        return 0;  /* Optional */
    }

    int idx = 0;
    json_object_object_foreach(knobs_obj, k, v) {
        if (idx >= MAX_KNOBS) {
            log_warn("sysfs: too many knobs (max %d), truncating", MAX_KNOBS);
            break;
        }
        KnobDef *knob = &cfg->sysfs.knobs[idx];
        strncpy(knob->name, k, MAX_NAME_LEN - 1);
        knob->name[MAX_NAME_LEN - 1] = '\0';
        strncpy(knob->path, json_object_get_string(v), MAX_PATH_LEN - 1);
        knob->path[MAX_PATH_LEN - 1] = '\0';
        knob->enabled = true;
        idx++;
        log_debug("sysfs knob[%d]: %s = %s", idx - 1, knob->name, knob->path);
    }
    cfg->sysfs.nr_knobs = idx;
    return 0;
}

/* ----------------------------------------------------------------
 * Sched rules parsing
 * ---------------------------------------------------------------- */

static int parse_sched_cpumasks(struct json_object *mod_obj, Config *cfg) {
    struct json_object *masks = NULL;
    cfg->sched.nr_cpumasks = 0;
    if (!json_object_object_get_ex(mod_obj, "cpumask", &masks)) return 0;
    if (!json_object_is_type(masks, json_type_object)) {
        log_error("sched.cpumask must be an object");
        return -1;
    }

    json_object_object_foreach(masks, name, value) {
        if (cfg->sched.nr_cpumasks >= MAX_CPU_MASK_GROUPS) {
            log_error("Too many sched cpumask groups (maximum %d)",
                      MAX_CPU_MASK_GROUPS);
            return -1;
        }
        CpuMaskGroup *group =
            &cfg->sched.cpumasks[cfg->sched.nr_cpumasks];
        memset(group, 0, sizeof(*group));
        snprintf(group->name, sizeof(group->name), "%s", name);
        if (parse_cpu_array(value, &group->mask, group->cpus,
                            &group->nr_cpus) < 0) {
            log_error("sched.cpumask.%s must contain unique CPUs 0-%d",
                      name, MAX_CPUS - 1);
            return -1;
        }
        cfg->sched.nr_cpumasks++;
    }
    return 0;
}

static int resolve_power_model_cpumasks(Config *cfg) {
    uint64_t assigned = 0;
    for (int i = 0; i < cfg->cpu.nr_clusters; i++) {
        PowerModelEntry *pm = &cfg->cpu.power_model[i];
        if (pm->cpu_mask == 0 && pm->cpumask_name[0] != '\0') {
            for (int j = 0; j < cfg->sched.nr_cpumasks; j++) {
                if (strcmp(pm->cpumask_name,
                           cfg->sched.cpumasks[j].name) == 0) {
                    pm->cpu_mask = cfg->sched.cpumasks[j].mask;
                    break;
                }
            }
        }
        if (!cfg->cpu.enable && pm->cpu_mask == 0) continue;
        if (pm->cpu_mask == 0) {
            log_error("powerModel[%d] needs 'cpus' or a valid 'cpumask'", i);
            return -1;
        }
        int mask_cpus = __builtin_popcountll(pm->cpu_mask);
        if (mask_cpus != pm->nr_cores) {
            log_error("powerModel[%d] mask has %d CPUs but nr=%d", i,
                      mask_cpus, pm->nr_cores);
            return -1;
        }
        if ((assigned & pm->cpu_mask) != 0) {
            log_error("powerModel[%d] CPU mask overlaps an earlier model", i);
            return -1;
        }
        assigned |= pm->cpu_mask;
    }
    return 0;
}

static const char *SCHED_CONTEXT_NAMES[SCHED_CTX_NUM] = {
    "bg", "fg", "idle", "touch", "boost"
};

static int find_cpumask(const SchedConfig *sched, const char *name) {
    if (!sched || !name) return -1;
    for (int i = 0; i < sched->nr_cpumasks; i++) {
        if (strcmp(sched->cpumasks[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_affinity_profile(const SchedConfig *sched, const char *name) {
    if (!sched || !name) return -1;
    for (int i = 0; i < sched->nr_affinity_profiles; i++) {
        if (strcmp(sched->affinity_profiles[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_priority_profile(const SchedConfig *sched, const char *name) {
    if (!sched || !name) return -1;
    for (int i = 0; i < sched->nr_priority_profiles; i++) {
        if (strcmp(sched->priority_profiles[i].name, name) == 0) return i;
    }
    return -1;
}

static int validate_regex(const char *pattern, const char *what) {
    regex_t compiled;
    int result = regcomp(&compiled, pattern, REG_EXTENDED | REG_NOSUB);
    if (result == 0) {
        regfree(&compiled);
        return 0;
    }
    char error[160];
    regerror(result, &compiled, error, sizeof(error));
    log_error("Invalid %s regex '%s': %s", what, pattern, error);
    return -1;
}

static int parse_sched_affinity(struct json_object *mod_obj, Config *cfg) {
    struct json_object *profiles = NULL;
    cfg->sched.nr_affinity_profiles = 0;
    if (!json_object_object_get_ex(mod_obj, "affinity", &profiles)) return 0;
    if (!json_object_is_type(profiles, json_type_object)) {
        log_error("sched.affinity must be an object");
        return -1;
    }

    json_object_object_foreach(profiles, name, value) {
        if (cfg->sched.nr_affinity_profiles >= MAX_AFFINITY_PROFILES) {
            log_error("Too many sched affinity profiles (maximum %d)",
                      MAX_AFFINITY_PROFILES);
            return -1;
        }
        if (!json_object_is_type(value, json_type_object)) {
            log_error("sched.affinity.%s must be an object", name);
            return -1;
        }
        AffinityProfile *profile =
            &cfg->sched.affinity_profiles[cfg->sched.nr_affinity_profiles];
        memset(profile, 0, sizeof(*profile));
        snprintf(profile->name, sizeof(profile->name), "%s", name);
        for (int context = 0; context < SCHED_CTX_NUM; context++) {
            struct json_object *mask_value = NULL;
            if (!json_object_object_get_ex(value,
                                           SCHED_CONTEXT_NAMES[context],
                                           &mask_value))
                continue;
            if (!json_object_is_type(mask_value, json_type_string)) {
                log_error("sched.affinity.%s.%s must be a string", name,
                          SCHED_CONTEXT_NAMES[context]);
                return -1;
            }
            const char *mask_name = json_object_get_string(mask_value);
            if (!mask_name || mask_name[0] == '\0') continue;
            int index = find_cpumask(&cfg->sched, mask_name);
            if (index < 0) {
                log_error("sched.affinity.%s.%s references unknown cpumask '%s'",
                          name, SCHED_CONTEXT_NAMES[context], mask_name);
                return -1;
            }
            profile->masks[context] = cfg->sched.cpumasks[index].mask;
            profile->has_mask[context] = true;
        }
        cfg->sched.nr_affinity_profiles++;
    }
    return 0;
}

static bool valid_priority_value(int value) {
    return value == 0 || value == -1 || value == -2 || value == -3 ||
           (value >= 1 && value <= 98) ||
           (value >= 100 && value <= 139);
}

static int parse_sched_priorities(struct json_object *mod_obj, Config *cfg) {
    struct json_object *profiles = NULL;
    cfg->sched.nr_priority_profiles = 0;
    if (!json_object_object_get_ex(mod_obj, "prio", &profiles)) return 0;
    if (!json_object_is_type(profiles, json_type_object)) {
        log_error("sched.prio must be an object");
        return -1;
    }

    json_object_object_foreach(profiles, name, value) {
        if (cfg->sched.nr_priority_profiles >= MAX_PRIORITY_PROFILES) {
            log_error("Too many sched priority profiles (maximum %d)",
                      MAX_PRIORITY_PROFILES);
            return -1;
        }
        if (!json_object_is_type(value, json_type_object)) {
            log_error("sched.prio.%s must be an object", name);
            return -1;
        }
        PriorityProfile *profile =
            &cfg->sched.priority_profiles[cfg->sched.nr_priority_profiles];
        memset(profile, 0, sizeof(*profile));
        snprintf(profile->name, sizeof(profile->name), "%s", name);
        for (int context = 0; context < SCHED_CTX_NUM; context++) {
            struct json_object *priority = NULL;
            if (!json_object_object_get_ex(value,
                                           SCHED_CONTEXT_NAMES[context],
                                           &priority))
                continue;
            if (!json_object_is_type(priority, json_type_int)) {
                log_error("sched.prio.%s.%s must be an integer", name,
                          SCHED_CONTEXT_NAMES[context]);
                return -1;
            }
            int encoded = json_object_get_int(priority);
            if (!valid_priority_value(encoded)) {
                log_error("sched.prio.%s.%s has invalid value %d", name,
                          SCHED_CONTEXT_NAMES[context], encoded);
                return -1;
            }
            profile->values[context] = encoded;
        }
        cfg->sched.nr_priority_profiles++;
    }
    return 0;
}

static int find_cgroup_class(const CgroupConfig *cgroup, const char *name) {
    if (!cgroup || !name) return -1;
    for (int i = 0; i < cgroup->nr_classes; i++) {
        if (strcmp(cgroup->classes[i].name, name) == 0) return i;
    }
    return -1;
}

static int parse_cgroup_config(struct json_object *modules_obj, Config *cfg) {
    struct json_object *cgroup_obj = NULL;
    cfg->cgroup.enable = false;
    snprintf(cfg->cgroup.backend, sizeof(cfg->cgroup.backend), "systemd");
    if (!json_object_object_get_ex(modules_obj, "cgroup", &cgroup_obj))
        return 0;
    if (!json_object_is_type(cgroup_obj, json_type_object)) {
        log_error("modules.cgroup must be an object");
        return -1;
    }
    cfg->cgroup.enable = parse_bool_field(cgroup_obj, "enable", true);
    const char *backend = NULL;
    if (parse_string_field(cgroup_obj, "backend", &backend) == 0 && backend)
        snprintf(cfg->cgroup.backend, sizeof(cfg->cgroup.backend), "%s",
                 backend);

    struct json_object *classes = NULL;
    if (!json_object_object_get_ex(cgroup_obj, "classes", &classes)) return 0;
    if (!json_object_is_type(classes, json_type_object)) {
        log_error("cgroup.classes must be an object");
        return -1;
    }
    json_object_object_foreach(classes, name, value) {
        if (cfg->cgroup.nr_classes >= MAX_CGROUP_CLASSES) {
            log_error("Too many cgroup classes (maximum %d)",
                      MAX_CGROUP_CLASSES);
            return -1;
        }
        if (!json_object_is_type(value, json_type_object)) {
            log_error("cgroup.classes.%s must be an object", name);
            return -1;
        }
        CgroupClass *klass = &cfg->cgroup.classes[cfg->cgroup.nr_classes];
        memset(klass, 0, sizeof(*klass));
        snprintf(klass->name, sizeof(klass->name), "%s", name);
        klass->cpu_weight = 100;
        klass->uclamp_max = 1024;
        const char *mask_name = NULL;
        if (parse_string_field(value, "cpumask", &mask_name) == 0 &&
            mask_name && mask_name[0] != '\0') {
            int index = find_cpumask(&cfg->sched, mask_name);
            if (index < 0) {
                log_error("cgroup.classes.%s references unknown cpumask '%s'",
                          name, mask_name);
                return -1;
            }
            snprintf(klass->cpumask_name, sizeof(klass->cpumask_name), "%s",
                     mask_name);
            klass->cpu_mask = cfg->sched.cpumasks[index].mask;
        }
        parse_int_field(value, "cpuWeight", &klass->cpu_weight);
        parse_int_field(value, "uclampMin", &klass->uclamp_min);
        parse_int_field(value, "uclampMax", &klass->uclamp_max);
        if (klass->cpu_weight < 1 || klass->cpu_weight > 10000 ||
            klass->uclamp_min < 0 || klass->uclamp_min > 1024 ||
            klass->uclamp_max < 0 || klass->uclamp_max > 1024 ||
            klass->uclamp_min > klass->uclamp_max) {
            log_error("cgroup.classes.%s has invalid weight/uClamp values",
                      name);
            return -1;
        }
        cfg->cgroup.nr_classes++;
    }
    return 0;
}

static int parse_sched_rules(struct json_object *mod_obj, Config *cfg) {
    struct json_object *rules_arr = NULL;
    if (!json_object_object_get_ex(mod_obj, "rules", &rules_arr))
        return 0;  /* Optional */
    if (!json_object_is_type(rules_arr, json_type_array)) {
        log_error("sched.rules must be an array");
        return -1;
    }

    int nr = json_object_array_length(rules_arr);
    if (nr > MAX_RULES) {
        log_error("Too many sched process rules (maximum %d)", MAX_RULES);
        return -1;
    }

    cfg->sched.nr_rules = 0;
    for (int i = 0; i < nr; i++) {
        struct json_object *rule_obj = json_object_array_get_idx(rules_arr, i);
        if (!json_object_is_type(rule_obj, json_type_object)) {
            log_error("sched.rules[%d] must be an object", i);
            return -1;
        }
        SchedRule *rule = &cfg->sched.rules[cfg->sched.nr_rules];
        memset(rule, 0, sizeof(*rule));

        const char *name_str = NULL;
        parse_string_field(rule_obj, "name", &name_str);
        if (name_str) snprintf(rule->name, sizeof(rule->name), "%s", name_str);

        const char *regex_str = NULL;
        parse_string_field(rule_obj, "regex", &regex_str);
        if (regex_str)
            snprintf(rule->regex, sizeof(rule->regex), "%s", regex_str);

        rule->pinned = parse_bool_field(rule_obj, "pinned", false);
        rule->match_game = parse_bool_field(rule_obj, "game", false);
        const char *cgroup_name = NULL;
        if (parse_string_field(rule_obj, "cgroup", &cgroup_name) == 0 &&
            cgroup_name)
            snprintf(rule->cgroup_class, sizeof(rule->cgroup_class), "%s",
                     cgroup_name);
        if (rule->regex[0] == '\0' && !rule->match_game) {
            log_error("sched.rules[%d] needs a process regex or game=true", i);
            return -1;
        }
        if (rule->regex[0] != '\0' &&
            validate_regex(rule->regex, "process") < 0)
            return -1;
        if (rule->cgroup_class[0] != '\0' &&
            find_cgroup_class(&cfg->cgroup, rule->cgroup_class) < 0) {
            log_error("sched.rules[%d] references unknown cgroup class '%s'",
                      i, rule->cgroup_class);
            return -1;
        }

        struct json_object *nested = NULL;
        if (json_object_object_get_ex(rule_obj, "rules", &nested)) {
            if (!json_object_is_type(nested, json_type_array)) {
                log_error("sched.rules[%d].rules must be an array", i);
                return -1;
            }
            int nn = json_object_array_length(nested);
            if (nn > MAX_THREAD_RULES) {
                log_error("sched.rules[%d] has more than %d thread rules", i,
                          MAX_THREAD_RULES);
                return -1;
            }
            for (int j = 0; j < nn; j++) {
                struct json_object *thread_obj =
                    json_object_array_get_idx(nested, j);
                if (!json_object_is_type(thread_obj, json_type_object)) {
                    log_error("sched.rules[%d].rules[%d] must be an object", i,
                              j);
                    return -1;
                }
                SchedThreadRule *thread = &rule->thread_rules[j];
                memset(thread, 0, sizeof(*thread));
                thread->affinity_index = -1;
                thread->priority_index = -1;
                const char *thread_regex = NULL;
                parse_string_field(thread_obj, "k", &thread_regex);
                if (!thread_regex || thread_regex[0] == '\0') {
                    log_error("sched.rules[%d].rules[%d] needs regex 'k'", i,
                              j);
                    return -1;
                }
                snprintf(thread->regex, sizeof(thread->regex), "%s",
                         thread_regex);
                if (strcmp(thread_regex, "/MAIN_THREAD/") != 0 &&
                    validate_regex(thread_regex, "thread") < 0)
                    return -1;
                const char *ac_str = NULL;
                parse_string_field(thread_obj, "ac", &ac_str);
                if (ac_str && ac_str[0] != '\0') {
                    thread->affinity_index =
                        find_affinity_profile(&cfg->sched, ac_str);
                    if (thread->affinity_index < 0) {
                        log_error("sched.rules[%d].rules[%d] references "
                                  "unknown affinity '%s'", i, j, ac_str);
                        return -1;
                    }
                    snprintf(thread->affinity_name,
                             sizeof(thread->affinity_name), "%s", ac_str);
                }
                const char *pc_str = NULL;
                parse_string_field(thread_obj, "pc", &pc_str);
                if (pc_str && pc_str[0] != '\0') {
                    thread->priority_index =
                        find_priority_profile(&cfg->sched, pc_str);
                    if (thread->priority_index < 0) {
                        log_error("sched.rules[%d].rules[%d] references "
                                  "unknown priority '%s'", i, j, pc_str);
                        return -1;
                    }
                    snprintf(thread->priority_name,
                             sizeof(thread->priority_name), "%s", pc_str);
                }
                rule->nr_thread_rules++;
            }
        }
        if (rule->nr_thread_rules == 0) {
            log_error("sched.rules[%d] needs at least one thread rule", i);
            return -1;
        }
        cfg->sched.nr_rules++;
        log_debug("sched rule[%d]: %s regex=%s game=%d pinned=%d threads=%d",
                  i, rule->name, rule->regex, rule->match_game, rule->pinned,
                  rule->nr_thread_rules);
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Presets (balance/powersave/performance) parsing
 * ---------------------------------------------------------------- */

static PowerMode mode_from_string(const char *s) {
    if (!s) return MODE_NUM;
    if (strcmp(s, "balance") == 0)       return MODE_BALANCE;
    if (strcmp(s, "powersave") == 0)     return MODE_POWERSAVE;
    if (strcmp(s, "performance") == 0)   return MODE_PERFORMANCE;
    if (strcmp(s, "fast") == 0)          return MODE_FAST;
    return MODE_NUM;
}

static SceneState scene_from_string(const char *s) {
    if (!s) return SCENE_NUM_STATES;
    if (strcmp(s, "idle") == 0)      return SCENE_IDLE;
    if (strcmp(s, "touch") == 0)     return SCENE_TOUCH;
    if (strcmp(s, "trigger") == 0)   return SCENE_TRIGGER;
    if (strcmp(s, "gesture") == 0)   return SCENE_GESTURE;
    if (strcmp(s, "junk") == 0)      return SCENE_JUNK;
    if (strcmp(s, "switch") == 0)    return SCENE_SWITCH;
    if (strcmp(s, "boost") == 0)     return SCENE_BOOST;
    return SCENE_NUM_STATES;
}

static int parse_presets(struct json_object *cfg_obj, Config *out) {
    struct json_object *presets_obj;
    if (!json_object_object_get_ex(cfg_obj, "presets", &presets_obj)) {
        log_error("Missing 'presets' object in config");
        return -1;
    }

    /* Iterate each power mode preset */
    json_object_object_foreach(presets_obj, mode_name, mode_obj) {
        PowerMode mode = mode_from_string(mode_name);
        if (mode >= MODE_NUM) {
            log_warn("Unknown preset mode: %s, skipping", mode_name);
            continue;
        }

        PowerModePreset *preset = &out->presets[mode];
        strncpy(preset->name, mode_name, MAX_NAME_LEN - 1);

        /* Each mode has state sub-objects: idle, touch, trigger, etc. */
        json_object_object_foreach(mode_obj, state_name, state_obj) {
            /* Parse global defaults (*) */
            if (strcmp(state_name, "*") == 0) {
                parse_action_params(state_obj, &preset->presets.global);
                continue;
            }

            /* Map state name to enum and get ActionParams pointer */
            SceneState scene = scene_from_string(state_name);
            if (scene >= SCENE_NUM_STATES) {
                log_warn("Unknown preset scene '%s' in mode '%s', skipping",
                         state_name, mode_name);
                continue;
            }
            parse_action_params(state_obj, get_state_action(&preset->presets, scene));
        }
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Initials parsing
 * ---------------------------------------------------------------- */

static int parse_initials(struct json_object *cfg_obj, Config *out) {
    out->initial_latency_time       = 0.2f;
    out->initial_slow_limit_power   = 3.0f;
    out->initial_fast_limit_power   = 6.0f;
    out->initial_fast_limit_capacity = 10.0f;
    out->initial_fast_limit_recover_scale = 0.3f;
    out->initial_margin             = 0.25f;
    out->initial_burst              = 0.0f;
    out->initial_guide_cap          = false;
    out->initial_limit_efficiency   = false;
    out->initial_base_sample_time   = 0.01f;
    out->initial_base_slack_time    = 0.01f;
    out->initial_predict_thd        = 0.3f;

    struct json_object *initials_obj;
    if (!json_object_object_get_ex(cfg_obj, "initials", &initials_obj))
        return 0;  /* Optional */

    struct json_object *cpu_obj;
    if (json_object_object_get_ex(initials_obj, "cpu", &cpu_obj)) {
        json_object_object_foreach(cpu_obj, k, v) {
            double d = json_object_get_double(v);
            if (strcmp(k, "latencyTime") == 0)       out->initial_latency_time = (float)d;
            else if (strcmp(k, "slowLimitPower") == 0) out->initial_slow_limit_power = (float)d;
            else if (strcmp(k, "fastLimitPower") == 0) out->initial_fast_limit_power = (float)d;
            else if (strcmp(k, "fastLimitCapacity") == 0) out->initial_fast_limit_capacity = (float)d;
            else if (strcmp(k, "fastLimitRecoverScale") == 0) out->initial_fast_limit_recover_scale = (float)d;
            else if (strcmp(k, "margin") == 0) out->initial_margin = (float)d;
            else if (strcmp(k, "burst") == 0) out->initial_burst = (float)d;
            else if (strcmp(k, "guideCap") == 0) out->initial_guide_cap = json_object_get_boolean(v);
            else if (strcmp(k, "limitEfficiency") == 0) out->initial_limit_efficiency = json_object_get_boolean(v);
            else if (strcmp(k, "baseSampleTime") == 0) out->initial_base_sample_time = (float)d;
            else if (strcmp(k, "baseSlackTime") == 0) out->initial_base_slack_time = (float)d;
            else if (strcmp(k, "predictThd") == 0) out->initial_predict_thd = (float)d;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Meta parsing
 * ---------------------------------------------------------------- */

static int parse_meta(struct json_object *cfg_obj, Config *out) {
    struct json_object *meta_obj;
    if (!json_object_object_get_ex(cfg_obj, "meta", &meta_obj))
        return 0;  /* Optional */

    struct json_object *name_jo = NULL;
    if (json_object_object_get_ex(meta_obj, "name", &name_jo)) {
        const char *name = json_object_get_string(name_jo);
        if (name)
            strncpy(out->meta_name, name, MAX_NAME_LEN - 1);
    }

    struct json_object *author_jo = NULL;
    if (json_object_object_get_ex(meta_obj, "author", &author_jo)) {
        const char *author = json_object_get_string(author_jo);
        if (author)
            strncpy(out->meta_author, author, MAX_NAME_LEN - 1);
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Switcher / Input module parsing
 * ---------------------------------------------------------------- */

static int parse_switcher(struct json_object *cfg_obj, Config *out) {
    struct json_object *mod_obj;
    if (!json_object_object_get_ex(cfg_obj, "modules", &mod_obj))
        return -1;

    struct json_object *sw_obj;
    if (json_object_object_get_ex(mod_obj, "switcher", &sw_obj)) {
        struct json_object *jo;
        const char *s;
        if (json_object_object_get_ex(sw_obj, "switchInode", &jo)) {
            s = json_object_get_string(jo);
            if (s) strncpy(out->switcher.switch_inode, s, MAX_PATH_LEN - 1);
        }
        if (json_object_object_get_ex(sw_obj, "perapp", &jo)) {
            s = json_object_get_string(jo);
            if (s) strncpy(out->switcher.perapp_file, s, MAX_PATH_LEN - 1);
        }

        /* hintDuration */
        struct json_object *hd;
        if (json_object_object_get_ex(sw_obj, "hintDuration", &hd)) {
            json_object_object_foreach(hd, sn, sv) {
                SceneState scene = scene_from_string(sn);
                if (scene < SCENE_NUM_STATES)
                    out->switcher.hint_duration[scene] = json_object_get_double(sv);
                else
                    log_warn("Unknown hintDuration scene '%s', skipping", sn);
            }
        }
    }

    /* Input module */
    struct json_object *inp_obj;
    if (json_object_object_get_ex(mod_obj, "input", &inp_obj)) {
        out->input.enable = parse_bool_field(inp_obj, "enable", true);
        out->input.swipe_thd = 0.03f;
        out->input.gesture_thd_x = 0.03f;
        out->input.gesture_thd_y = 0.03f;
        out->input.gesture_delay_time = 2.0f;
        out->input.hold_enter_time = 1.0f;

        struct json_object *jo;
        if (json_object_object_get_ex(inp_obj, "swipeThd", &jo))
            out->input.swipe_thd = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "gestureThdX", &jo))
            out->input.gesture_thd_x = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "gestureThdY", &jo))
            out->input.gesture_thd_y = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "gestureDelayTime", &jo))
            out->input.gesture_delay_time = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "holdEnterTime", &jo))
            out->input.hold_enter_time = json_object_get_double(jo);
        if (json_object_object_get_ex(inp_obj, "screen_width", &jo))
            out->input.screen_width = json_object_get_int(jo);
        if (json_object_object_get_ex(inp_obj, "screen_height", &jo))
            out->input.screen_height = json_object_get_int(jo);
    }

    /* Thermal module. Keep defaults when older configs omit it. */
    out->thermal.enable = true;
    out->thermal.warn_temp = 70000;
    out->thermal.throttle_temp = 80000;
    out->thermal.critical_temp = 95000;
    out->thermal.recovery_temp = 75000;
    struct json_object *thermal_obj;
    if (json_object_object_get_ex(mod_obj, "thermal", &thermal_obj)) {
        out->thermal.enable = parse_bool_field(
            thermal_obj, "enabled",
            parse_bool_field(thermal_obj, "enable", true));
        parse_int_field(thermal_obj, "warn_temp", &out->thermal.warn_temp);
        parse_int_field(thermal_obj, "throttle_temp",
                        &out->thermal.throttle_temp);
        parse_int_field(thermal_obj, "critical_temp",
                        &out->thermal.critical_temp);
        parse_int_field(thermal_obj, "recovery_temp",
                        &out->thermal.recovery_temp);
    }

    return 0;
}

/* ----------------------------------------------------------------
 * Main entry: config_load
 * ---------------------------------------------------------------- */

int config_load(Config *cfg, const char *path) {
    memset(cfg, 0, sizeof(*cfg));

    /* Defaults */
    cfg->switcher.hint_duration[SCENE_IDLE]      = 0.0f;
    cfg->switcher.hint_duration[SCENE_TOUCH]     = 4.0f;
    cfg->switcher.hint_duration[SCENE_TRIGGER]   = 0.03f;
    cfg->switcher.hint_duration[SCENE_GESTURE]   = 0.1f;
    cfg->switcher.hint_duration[SCENE_JUNK]      = 0.06f;
    cfg->switcher.hint_duration[SCENE_SWITCH]    = 0.4f;
    cfg->switcher.hint_duration[SCENE_BOOST]     = 0.0f;

    strncpy(cfg->switcher.switch_inode, "/run/uperf-linux/cur_powermode", MAX_PATH_LEN - 1);
    strncpy(cfg->switcher.perapp_file, "/etc/uperf-linux/perapp_powermode", MAX_PATH_LEN - 1);

    strncpy(cfg->meta_name, "unknown", MAX_NAME_LEN - 1);
    strncpy(cfg->meta_author, "unknown", MAX_NAME_LEN - 1);

    /* Parse JSON using json_object_from_file (replaces deprecated json_parse_file) */
    struct json_object *root = json_object_from_file(path);
    if (!root) {
        log_error("Failed to parse JSON config '%s': file not found or invalid JSON", path);
        return -1;
    }

    /* Resolve module objects once, then pass each parser the level it expects. */
    struct json_object *modules_obj = NULL;
    struct json_object *cpu_obj = NULL;
    struct json_object *sysfs_obj = NULL;
    struct json_object *sched_obj = NULL;

    if (!json_object_object_get_ex(root, "modules", &modules_obj)) {
        log_error("Missing 'modules' object in config");
        goto err;
    }
    if (!json_object_object_get_ex(modules_obj, "cpu", &cpu_obj)) {
        log_error("Missing 'modules.cpu' object in config");
        goto err;
    }
    if (!json_object_object_get_ex(modules_obj, "sysfs", &sysfs_obj)) {
        log_error("Missing 'modules.sysfs' object in config");
        goto err;
    }

    cfg->cpu.enable = parse_bool_field(cpu_obj, "enable", true);
    cfg->sysfs.enable = parse_bool_field(sysfs_obj, "enable", true);

    /* Walk the top-level structure. */
    if (parse_meta(root, cfg) < 0)               goto err;
    if (parse_switcher(root, cfg) < 0)           goto err;
    if (parse_power_model(cpu_obj, cfg) < 0)     goto err;
    if (parse_sysfs_knobs(sysfs_obj, cfg) < 0)   goto err;
    if (json_object_object_get_ex(modules_obj, "sched", &sched_obj)) {
        cfg->sched.enable = parse_bool_field(sched_obj, "enable", true);
        if (parse_sched_cpumasks(sched_obj, cfg) < 0) goto err;
        if (parse_sched_affinity(sched_obj, cfg) < 0) goto err;
        if (parse_sched_priorities(sched_obj, cfg) < 0) goto err;
    }
    if (parse_cgroup_config(modules_obj, cfg) < 0) goto err;
    if (sched_obj) {
        if (parse_sched_rules(sched_obj, cfg) < 0) goto err;
    }
    if (resolve_power_model_cpumasks(cfg) < 0) goto err;
    if (parse_presets(root, cfg) < 0)            goto err;
    if (parse_initials(root, cfg) < 0)           goto err;

    json_object_put(root);
    log_info("Config loaded: %s by %s (%d clusters, %d knobs, %d rules, "
             "%d cgroup classes)",
             cfg->meta_name, cfg->meta_author,
             cfg->cpu.nr_clusters, cfg->sysfs.nr_knobs, cfg->sched.nr_rules,
             cfg->cgroup.nr_classes);
    return 0;

err:
    json_object_put(root);
    log_error("Config parse failed for '%s'", path);
    return -1;
}

/* ----------------------------------------------------------------
 * config_validate
 * ---------------------------------------------------------------- */

int config_validate(const Config *cfg) {
    if (!cfg) {
        log_error("Validation failed: config is NULL");
        return -1;
    }

    if (cfg->cpu.nr_clusters <= 0 || cfg->cpu.nr_clusters > MAX_CLUSTERS) {
        log_error("Validation failed: cluster count must be between 1 and %d",
                  MAX_CLUSTERS);
        return -1;
    }

    for (int i = 0; i < cfg->cpu.nr_clusters; i++) {
        const PowerModelEntry *pm = &cfg->cpu.power_model[i];
        if (pm->nr_cores <= 0) {
            log_error("Validation failed: cluster %d nr must be > 0", i);
            return -1;
        }
        if (cfg->cpu.enable &&
            (pm->cpu_mask == 0 ||
             __builtin_popcountll(pm->cpu_mask) != pm->nr_cores)) {
            log_error("Validation failed: cluster %d needs an explicit CPU "
                      "mask containing exactly %d CPUs", i, pm->nr_cores);
            return -1;
        }
        if (pm->typical_freq_mhz <= 0) {
            log_error("Validation failed: cluster %d typicalFreq must be > 0", i);
            return -1;
        }
        if (pm->efficiency <= 0) {
            log_error("Validation failed: cluster %d efficiency must be > 0", i);
            return -1;
        }
        if (pm->typical_power_w <= 0.0f) {
            log_error("Validation failed: cluster %d typicalPower must be > 0", i);
            return -1;
        }
        if (pm->free_freq_mhz <= 0.0f ||
            pm->free_freq_mhz >= pm->plain_freq_mhz ||
            pm->plain_freq_mhz >= pm->sweet_freq_mhz ||
            pm->sweet_freq_mhz > pm->typical_freq_mhz) {
            log_error("Validation failed: cluster %d frequencies must satisfy "
                      "0 < freeFreq < plainFreq < sweetFreq <= typicalFreq", i);
            return -1;
        }
    }

    bool has_preset = false;
    for (int m = 0; m < MODE_NUM; m++) {
        if (cfg->presets[m].name[0] != '\0') {
            has_preset = true;
            break;
        }
    }
    if (!has_preset) {
        log_error("Validation failed: no power mode presets defined");
        return -1;
    }

    if (cfg->thermal.enable &&
        (cfg->thermal.warn_temp <= 0 ||
         cfg->thermal.warn_temp >= cfg->thermal.throttle_temp ||
         cfg->thermal.throttle_temp >= cfg->thermal.critical_temp ||
         cfg->thermal.recovery_temp <= 0 ||
         cfg->thermal.recovery_temp >= cfg->thermal.throttle_temp)) {
        log_error("Validation failed: invalid thermal thresholds");
        return -1;
    }

    if (cfg->sched.enable &&
        (cfg->sched.nr_cpumasks <= 0 ||
         cfg->sched.nr_affinity_profiles <= 0 ||
         cfg->sched.nr_priority_profiles <= 0 ||
         cfg->sched.nr_rules <= 0)) {
        log_error("Validation failed: enabled sched module needs cpumasks, "
                  "affinity/prio profiles and process rules");
        return -1;
    }

    if (cfg->cgroup.enable) {
        if (!cfg->sched.enable) {
            log_error("Validation failed: cgroup module requires sched module");
            return -1;
        }
        if (strcmp(cfg->cgroup.backend, "systemd") != 0) {
            log_error("Validation failed: unsupported cgroup backend '%s'",
                      cfg->cgroup.backend);
            return -1;
        }
        if (cfg->cgroup.nr_classes <= 0) {
            log_error("Validation failed: enabled cgroup module needs classes");
            return -1;
        }
    }

    log_info("Config validation passed (%d clusters, %d modes, sched=%d, "
             "cgroup=%d)", cfg->cpu.nr_clusters, MODE_NUM,
             cfg->sched.enable, cfg->cgroup.enable);
    return 0;
}

/* ----------------------------------------------------------------
 * config_check_paths
 * ---------------------------------------------------------------- */

int config_check_paths(const Config *cfg) {
    int missing = 0;
    int total = 0;

    for (int i = 0; i < cfg->sysfs.nr_knobs; i++) {
        const KnobDef *knob = &cfg->sysfs.knobs[i];
        if (!knob->enabled) continue;
        total++;

        char concrete_path[MAX_PATH_LEN];
        const char *path = knob->path;
        if (strstr(knob->path, "%d")) {
            int written = snprintf(concrete_path, sizeof(concrete_path),
                                   knob->path, 0);
            if (written < 0 || written >= (int)sizeof(concrete_path)) {
                log_error("Invalid formatted config path: %s (%s)",
                          knob->path, knob->name);
                return -1;
            }
            path = concrete_path;
        }

        int fd = open(path, O_WRONLY);
        if (fd < 0) {
            if (errno == ENOENT) {
                log_warn("Config path not found: %s (%s)", path, knob->name);
                missing++;
            } else {
                log_error("Cannot write to config path: %s (%s): %s",
                          path, knob->name, strerror(errno));
                return -1;
            }
        } else {
            close(fd);
        }
    }

    const char *dir = strrchr(cfg->switcher.switch_inode, '/');
    if (dir) {
        char dirpath[MAX_PATH_LEN];
        size_t dirlen = (size_t)(dir - cfg->switcher.switch_inode);
        if (dirlen >= MAX_PATH_LEN) dirlen = MAX_PATH_LEN - 1;
        strncpy(dirpath, cfg->switcher.switch_inode, dirlen);
        dirpath[dirlen] = '\0';
        struct stat st;
        if (stat(dirpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
            log_warn("Switch inode directory does not exist: %s (will be created at runtime)", dirpath);
        }
    }

    if (missing > 0) {
        log_warn("%d/%d sysfs paths missing (non-fatal: they may appear at runtime)",
                 missing, total);
    } else if (total > 0) {
        log_info("All %d sysfs paths verified OK", total);
    }

    return 0;
}

void config_free(Config *cfg) {
    (void)cfg;
}
