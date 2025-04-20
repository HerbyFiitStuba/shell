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
 * @brief Creates and configures a TCP listening socket.
 * @param cfg Pointer to the configuration (port, bind_addr).
 */
int socket_setup(const Config *cfg);

/**
 * @brief Starts the server loop, accepting and handling client connections.
 * @param cfg Pointer to the configuration.
 */
void server_run(const Config *cfg);

#endif // SERVER_H