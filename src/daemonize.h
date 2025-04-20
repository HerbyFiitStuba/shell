#ifndef DAEMONIZE_H
#define DAEMONIZE_H

/**
 * @brief Daemonizes the current process.
 *
 * Performs steps to detach the process from the controlling
 * terminal and run it in the background as a daemon.
 * This includes forking, creating a new session,
 * and closing/redirecting standard file descriptors.
 *
 * @return 0 on success, -1 on failure (error logged).
 *         Note: On success, the original parent process will have exited.
 */
int daemonize_process();

#endif // DAEMONIZE_H
