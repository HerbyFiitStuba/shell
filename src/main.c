#include "config.h"
#include "logger.h"
#include <stdio.h>

int main(int argc, char **argv) {
    Config cfg;
    init_default_config(&cfg);
    load_args(argc, argv, &cfg);

    // Print the configuration for debugging
    print_config(&cfg);

    if (log_init(cfg.log_path, cfg.verbose) < 0) return 1;
    log_info("Starting shell in %s mode on port %d",
             cfg.mode==0?"server":"client", cfg.port);
    log_debug("Verbose mode enabled");

    // TODO: bude nasledovat server_run() alebo client_run()

    log_close();
    return 0;
}