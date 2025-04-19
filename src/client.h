#ifndef CLIENT_H
#define CLIENT_H

#include "config.h" // Include config to use the Config struct

/**
 * @brief Runs the shell in client mode.
 * Connects to the server specified in the config, forwards stdin to the server,
 * and prints server responses to stdout.
 *
 * @param cfg Pointer to the configuration structure.
 * @return 0 on successful completion, -1 on error.
 */
int client_run(Config *cfg);

#endif // CLIENT_H