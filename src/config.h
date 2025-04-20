#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_BUFFER_SIZE 256

typedef struct {
    int   mode;       // 0 = server, 1 = client
    int   port;       // TCP port
    char  bind_addr[CONFIG_BUFFER_SIZE];  // IP address ("-i") or empty string
    int   verbose;    // verbose flag ("-v")
    char  log_path[CONFIG_BUFFER_SIZE];   // log file path ("-l") or empty string
    int   timeout_val; // Client inactivity timeout in seconds ("-t")
    int   daemonize;  // Daemonize flag ("-d")
} Config;

// Nastaví predvolené hodnoty do cfg
void init_default_config(Config *cfg);
// Parsuje -s, -c, -p, -i, -v, -l a -h (help a exit)
void load_args(int argc, char **argv, Config *cfg);
// print_config(cfg) vypíše aktuálnu konfiguráciu
void print_config(const Config *cfg);
// Uvoľní pamäť (ak je potrebná)
// void free_config_memory(Config *cfg); // Not needed in this version

#endif // CONFIG_H