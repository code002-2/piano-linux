#define _GNU_SOURCE
#include "frequency_state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int parse_positive_value(const char *text, long long *value) {
    if (!text || !value || text[0] == '\0') return -1;
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0) return -1;
    *value = parsed;
    return 0;
}

const FrequencyStateEntry *frequency_state_find(
    const FrequencyStateEntry *entries, size_t count,
    const char *min_path, const char *max_path) {
    if (!entries || !min_path || !max_path) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].min_path, min_path) == 0 &&
            strcmp(entries[i].max_path, max_path) == 0)
            return &entries[i];
    }
    return NULL;
}

int frequency_state_load(const char *path, FrequencyStateEntry *entries,
                         size_t capacity, size_t *count) {
    if (!path || !entries || capacity == 0 || !count)
        return FREQUENCY_STATE_ERROR;
    *count = 0;
    FILE *fp = fopen(path, "re");
    if (!fp)
        return errno == ENOENT ? FREQUENCY_STATE_NOT_FOUND
                              : FREQUENCY_STATE_ERROR;

    char *line = NULL;
    size_t line_capacity = 0;
    int result = FREQUENCY_STATE_OK;
    while (getline(&line, &line_capacity, fp) >= 0) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        if (*count >= capacity) {
            result = FREQUENCY_STATE_ERROR;
            break;
        }

        char *fields[4] = {0};
        char *cursor = line;
        for (int i = 0; i < 4; i++) {
            fields[i] = strsep(&cursor, "\t");
            if (!fields[i]) break;
        }
        long long minimum = 0, maximum = 0;
        if (!fields[0] || !fields[1] || !fields[2] || !fields[3] || cursor ||
            fields[0][0] != '/' || fields[1][0] != '/' ||
            strlen(fields[0]) >= FREQUENCY_STATE_PATH_LEN ||
            strlen(fields[1]) >= FREQUENCY_STATE_PATH_LEN ||
            strlen(fields[2]) >= FREQUENCY_STATE_VALUE_LEN ||
            strlen(fields[3]) >= FREQUENCY_STATE_VALUE_LEN ||
            parse_positive_value(fields[2], &minimum) < 0 ||
            parse_positive_value(fields[3], &maximum) < 0 ||
            minimum > maximum ||
            frequency_state_find(entries, *count, fields[0], fields[1])) {
            result = FREQUENCY_STATE_ERROR;
            break;
        }
        FrequencyStateEntry *entry = &entries[(*count)++];
        snprintf(entry->min_path, sizeof(entry->min_path), "%s", fields[0]);
        snprintf(entry->max_path, sizeof(entry->max_path), "%s", fields[1]);
        snprintf(entry->original_min, sizeof(entry->original_min), "%s",
                 fields[2]);
        snprintf(entry->original_max, sizeof(entry->original_max), "%s",
                 fields[3]);
    }

    free(line);
    if (ferror(fp)) result = FREQUENCY_STATE_ERROR;
    fclose(fp);
    if (*count == 0) result = FREQUENCY_STATE_ERROR;
    if (result != FREQUENCY_STATE_OK) *count = 0;
    return result;
}

int frequency_state_save(const char *path, const FrequencyStateEntry *entries,
                         size_t count) {
    if (!path || !entries || count == 0 ||
        count > FREQUENCY_STATE_MAX_ENTRIES)
        return FREQUENCY_STATE_ERROR;
    size_t path_len = strlen(path);
    if (path_len > FREQUENCY_STATE_PATH_LEN - 16)
        return FREQUENCY_STATE_ERROR;

    char temporary[FREQUENCY_STATE_PATH_LEN];
    snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary);
    if (fd < 0) return FREQUENCY_STATE_ERROR;
    fchmod(fd, 0600);
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        unlink(temporary);
        return FREQUENCY_STATE_ERROR;
    }

    int result = FREQUENCY_STATE_OK;
    for (size_t i = 0; i < count; i++) {
        long long minimum = 0, maximum = 0;
        if (strchr(entries[i].min_path, '\t') ||
            strchr(entries[i].max_path, '\t') ||
            parse_positive_value(entries[i].original_min, &minimum) < 0 ||
            parse_positive_value(entries[i].original_max, &maximum) < 0 ||
            minimum > maximum ||
            fprintf(fp, "%s\t%s\t%s\t%s\n", entries[i].min_path,
                    entries[i].max_path, entries[i].original_min,
                    entries[i].original_max) < 0) {
            result = FREQUENCY_STATE_ERROR;
            break;
        }
    }
    if (result == FREQUENCY_STATE_OK &&
        (fflush(fp) != 0 || fsync(fileno(fp)) != 0))
        result = FREQUENCY_STATE_ERROR;
    if (fclose(fp) != 0) result = FREQUENCY_STATE_ERROR;
    if (result == FREQUENCY_STATE_OK && rename(temporary, path) != 0)
        result = FREQUENCY_STATE_ERROR;
    if (result != FREQUENCY_STATE_OK) unlink(temporary);
    return result;
}

int frequency_state_clear(const char *path) {
    if (!path) return FREQUENCY_STATE_ERROR;
    if (unlink(path) == 0 || errno == ENOENT) return FREQUENCY_STATE_OK;
    return FREQUENCY_STATE_ERROR;
}
