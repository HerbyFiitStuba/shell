#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h> // For errno with strtol
#include <limits.h> // For INT_MAX, LONG_MIN, LONG_MAX with strtol
#include <getopt.h> // For getopt() and associated variables like optarg

// Note: getopt is usually included via unistd.h or getopt.h on POSIX systems

// These are typically declared by including unistd.h or getopt.h
// extern char *optarg;
// extern int optind, opterr, optopt;

void init_default_config(Config *cfg) {
    cfg->mode      = 0;    // Default to server mode
    cfg->port      = 0;    // (0 = none/invalid until set)
    cfg->bind_addr[0] = '\0'; // Default to empty string (implies all interfaces)
    cfg->verbose   = 0;    // Default to verbose off
    cfg->log_path[0]  = '\0'; // Default to empty string (implies stderr)
    cfg->timeout_val = 0;   // Default timeout not set by arg (0 seconds)
    cfg->daemonize = 0;   // Default: do not daemonize
}

// Removed free_config_memory function

void load_args(int argc, char **argv, Config *cfg) {
    int opt;
    int mode_set = 0; // Flag to check if -s or -c was provided

    // opterr = 0; // Uncomment to disable getopt's default error messages

    // Add 'd' to the optstring
    while ((opt = getopt(argc, argv, "hscp:i:vl:t:d")) != -1) {
        switch (opt) {
            case 'h':
                // Update help message
                printf("Usage: %s [-s|-c] [-p port] [-i bind_addr] [-v] [-l logfile] [-t timeout] [-d] [-h]\n", argv[0]);
                printf("  -s : Run in server mode (default)\n");
                printf("  -c : Run in client mode\n");
                printf("  -p port : Port number (required for server, optional for client)\n");
                printf("  -i addr : IP address to bind server to (default: all interfaces)\n");
                printf("  -v : Enable verbose output\n");
                printf("  -l file : Log output to specified file (default: stderr)\n");
                printf("  -t secs : Client inactivity timeout in seconds (default: 600)\n");
                printf("  -d : Run server as a daemon process\n"); // Added daemon option
                printf("  -h : Display this help message\n");
                exit(0);
            case 's':
                if (mode_set && cfg->mode != 0) {
                    fprintf(stderr, "Error: Options -s and -c are mutually exclusive.\n");
                    exit(1);
                }
                cfg->mode = 0; // Server mode
                mode_set = 1;
                break;
            case 'c':
                 if (mode_set && cfg->mode != 1) {
                    fprintf(stderr, "Error: Options -s and -c are mutually exclusive.\n");
                    exit(1);
                }
                cfg->mode = 1; // Client mode
                mode_set = 1;
                break;
            case 'p': {
                char *endptr;
                long val;
                errno = 0; // Reset errno before call
                val = strtol(optarg, &endptr, 10);  // Convert string to long

                // Check for errors: empty string, non-numeric chars, out of range
                if (errno != 0 || endptr == optarg || *endptr != '\0' || val <= 0 || val > 65535) {
                     fprintf(stderr, "Error: Invalid port number '%s'. Must be between 1 and 65535.\n", optarg);
                     exit(1);
                }
                cfg->port = (int)val;
                break;
            }
            case 'i': {
                int n = snprintf(cfg->bind_addr, sizeof(cfg->bind_addr), "%s", optarg);
                // Check for snprintf errors
                if (n < 0) {
                    perror("Error formatting bind address");
                    // Optionally clear the buffer or handle error more gracefully
                    cfg->bind_addr[0] = '\0';
                    // Or exit(1); depending on desired behavior
                }
                // Check for truncation (snprintf returns the number of chars that *would* have been written)
                else if (n >= (int)sizeof(cfg->bind_addr)) {
                    fprintf(stderr, "Warning: Bind address truncated to %zu bytes.\n", sizeof(cfg->bind_addr) - 1);
                    // snprintf already null-terminated at cfg->bind_addr[sizeof(cfg->bind_addr) - 1]
                }
                // If 0 <= n < sizeof(cfg->bind_addr), snprintf succeeded and null-terminated at cfg->bind_addr[n]
                break;
            }
            case 'v':
                cfg->verbose = 1;
                break;
            case 'l': {
                 int n = snprintf(cfg->log_path, sizeof(cfg->log_path), "%s", optarg);
                 // Check for snprintf errors
                 if (n < 0) {
                     perror("Error formatting log path");
                     cfg->log_path[0] = '\0';
                     // Or exit(1);
                 }
                 // Check for truncation
                 else if (n >= (int)sizeof(cfg->log_path)) {
                     fprintf(stderr, "Warning: Log path truncated to %zu bytes.\n", sizeof(cfg->log_path) - 1);
                     // snprintf already null-terminated at cfg->log_path[sizeof(cfg->log_path) - 1]
                 }
                 // If 0 <= n < sizeof(cfg->log_path), snprintf succeeded and null-terminated at cfg->log_path[n]
                break;
            }
            case 't': {
                char *endptr;
                long val;
                errno = 0; // Reset errno before call
                val = strtol(optarg, &endptr, 10);

                // Check for errors: empty string, non-numeric chars, out of range (allow 0)
                if (errno != 0 || endptr == optarg || *endptr != '\0' || val < 0 || val > INT_MAX) {
                     fprintf(stderr, "Error: Invalid timeout value '%s'. Must be a non-negative integer.\n", optarg);
                     exit(1);
                }
                cfg->timeout_val = (int)val;
                break;
            }
            case 'd':
                cfg->daemonize = 1;
                break;
            case '?': // getopt reports invalid option or missing argument
                 // Error message is printed by getopt unless opterr is 0
                 fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
                 exit(1);
            default:
                // Should not happen with the given optstring
                fprintf(stderr, "Internal error: Unexpected option character code: %d\n", opt);
                exit(1);
        }
    }

    // Example: Check for required arguments after parsing
    if (cfg->mode == 0 && cfg->port == 0) { // Server mode requires a port
        fprintf(stderr, "Error: Port number (-p) is required for server mode.\n");
        fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
        exit(1);
    }

    // Daemon mode only makes sense for server mode
    if (cfg->daemonize && cfg->mode != 0) {
        fprintf(stderr, "Error: Daemon mode (-d) can only be used with server mode (-s or default).\n");
        exit(1);
    }

    // Handle non-option arguments (if any)
    // for (int i = optind; i < argc; i++) {
    //     printf("Non-option argument: %s\n", argv[i]);
    // }
}

void print_config(const Config *cfg) {
    printf("Configuration:\n");
    printf("  Mode: %s\n", cfg->mode == 0 ? "Server" : "Client");
    printf("  Port: %d\n", cfg->port);
    printf("  Bind Address: %s\n", cfg->bind_addr[0] != '\0' ? cfg->bind_addr : "All interfaces");
    printf("  Verbose: %s\n", cfg->verbose ? "Enabled" : "Disabled");
    printf("  Log Path: %s\n", cfg->log_path[0] != '\0' ? cfg->log_path : "stderr");
    printf("  Timeout: %d seconds\n", cfg->timeout_val); // Print timeout
    printf("  Daemonize: %s\n", cfg->daemonize ? "Enabled" : "Disabled"); // Print daemonize status
}
// Note: This function is for debugging purposes and can be removed in production
// or replaced with a more sophisticated logging mechanism.