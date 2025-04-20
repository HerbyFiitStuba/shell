#include "config.h"
#include "logger.h"
#include "server.h"
#include "client.h"
#include "daemonize.h"
#include <stdio.h>

#define DEFAULT_TIMEOUT 10 * 60 // Default timeout 10 minutes

int main(int argc, char **argv) {
    Config cfg;
    init_default_config(&cfg);
    load_args(argc, argv, &cfg);

    // Set default timeout if not specified by -t
    if (cfg.timeout_val == 0 && cfg.mode == 0) { // Only apply default in server mode
        cfg.timeout_val = DEFAULT_TIMEOUT;
        log_info("Info: Using default client timeout: %d seconds\n", cfg.timeout_val);
    }

    // Print the configuration for debugging
    print_config(&cfg);

    if (log_init(cfg.log_path, cfg.verbose) < 0) return 1;
    log_info("Starting shell in %s mode on port %d",
             cfg.mode==0?"server":"client", cfg.port);
    log_debug("Verbose mode enabled");

    // If daemonize is set, fork the process
    if (cfg.daemonize && cfg.mode == 0) { // Only daemonize in server mode
        log_info("Daemonizing process...");
        if (daemonize_process() < 0) {
            log_error("Failed to daemonize process");
            return 1;
        }
        log_info("Daemon process started successfully.");
    }
    
    // Run the server or client based on the mode

    if (cfg.mode == 0) {
        server_run(&cfg);
    } else {
        client_run(&cfg); 
    }

    log_close();
    return 0;
}