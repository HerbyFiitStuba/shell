#ifndef SERVER_H
#define SERVER_H

#include "config.h"

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