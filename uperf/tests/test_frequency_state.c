#include "frequency_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/uperf-frequency-state-test";
    FILE *fp = NULL;
    unlink(path);
    FrequencyStateEntry saved[2] = {
        {
            .min_path = "/sys/policy0/min",
            .max_path = "/sys/policy0/max",
            .original_min = "307200",
            .original_max = "1800000",
        },
        {
            .min_path = "/sys/gpu/min",
            .max_path = "/sys/gpu/max",
            .original_min = "220000000",
            .original_max = "680000000",
        },
    };
    assert(frequency_state_save(path, saved, 2) == FREQUENCY_STATE_OK);

    FrequencyStateEntry loaded[FREQUENCY_STATE_MAX_ENTRIES] = {0};
    size_t count = 0;
    assert(frequency_state_load(path, loaded,
                                FREQUENCY_STATE_MAX_ENTRIES, &count) ==
           FREQUENCY_STATE_OK);
    assert(count == 2);
    const FrequencyStateEntry *cpu = frequency_state_find(
        loaded, count, "/sys/policy0/min", "/sys/policy0/max");
    assert(cpu && strcmp(cpu->original_min, "307200") == 0);
    assert(strcmp(cpu->original_max, "1800000") == 0);

    /* A recovery file must reject duplicate path pairs, otherwise a damaged
     * snapshot could make restart behavior depend on entry order. */
    assert(frequency_state_save(path, saved, 2) == FREQUENCY_STATE_OK);
    fp = fopen(path, "a");
    assert(fp);
    fputs("/sys/policy0/min\t/sys/policy0/max\t307200\t1800000\n", fp);
    fclose(fp);
    count = 0;
    assert(frequency_state_load(path, loaded,
                                FREQUENCY_STATE_MAX_ENTRIES, &count) ==
           FREQUENCY_STATE_ERROR);
    assert(count == 0);

    fp = fopen(path, "w");
    assert(fp);
    fputs("invalid\n", fp);
    fclose(fp);
    count = 123;
    assert(frequency_state_load(path, loaded,
                                FREQUENCY_STATE_MAX_ENTRIES, &count) ==
           FREQUENCY_STATE_ERROR);
    assert(count == 0);
    assert(frequency_state_clear(path) == FREQUENCY_STATE_OK);
    assert(frequency_state_load(path, loaded,
                                FREQUENCY_STATE_MAX_ENTRIES, &count) ==
           FREQUENCY_STATE_NOT_FOUND);
    puts("frequency state tests passed");
    return 0;
}
