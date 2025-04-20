#ifndef SERVER_H
#define SERVER_H

#include "config.h"
#include <sys/types.h>  // For size_t
#include <time.h>       // For time_t

// Structure to hold state for each client, include for size_t
typedef struct {
    int fd;
    char *buffer;
    size_t buffer_size;     // Current number of bytes stored
    size_t buffer_capacity; // Current allocated capacity
    time_t last_activity;   // Timestamp of the last read activity
} client_state;

/**
 * @brief Vytvorí a nakonfiguruje TCP posluchový socket.
 * @param cfg Ukazovateľ na konfiguráciu (port, bind_addr).
 * @return fd socketu >=0, alebo -1 pri chybe (log_error už zaloguje).
 */
int socket_setup(const Config *cfg);

/**
 * @brief Spustí serverovú slučku: accept, read-line, echo, close.
 * @param cfg Ukazovateľ na konfiguráciu.
 */
void server_run(const Config *cfg);

#endif // SERVER_H