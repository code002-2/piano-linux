#ifndef UPERF_FREQUENCY_STATE_H
#define UPERF_FREQUENCY_STATE_H

#include <stddef.h>

#define FREQUENCY_STATE_MAX_ENTRIES 8
#define FREQUENCY_STATE_PATH_LEN 256
#define FREQUENCY_STATE_VALUE_LEN 32

typedef struct {
    char min_path[FREQUENCY_STATE_PATH_LEN];
    char max_path[FREQUENCY_STATE_PATH_LEN];
    char original_min[FREQUENCY_STATE_VALUE_LEN];
    char original_max[FREQUENCY_STATE_VALUE_LEN];
} FrequencyStateEntry;

enum {
    FREQUENCY_STATE_OK = 0,
    FREQUENCY_STATE_NOT_FOUND = 1,
    FREQUENCY_STATE_ERROR = -1,
};

int frequency_state_load(const char *path, FrequencyStateEntry *entries,
                         size_t capacity, size_t *count);
int frequency_state_save(const char *path, const FrequencyStateEntry *entries,
                         size_t count);
const FrequencyStateEntry *frequency_state_find(
    const FrequencyStateEntry *entries, size_t count,
    const char *min_path, const char *max_path);
int frequency_state_clear(const char *path);

#endif
