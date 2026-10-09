#include "config.h"
#include "log.h"

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s CONFIG\n", argv[0]);
        return 2;
    }
    log_init(UPERF_LOG_WARN, 0, NULL);
    Config config;
    int result = config_load(&config, argv[1]);
    if (result == 0) result = config_validate(&config);
    config_free(&config);
    log_shutdown();
    return result == 0 ? 0 : 1;
}
