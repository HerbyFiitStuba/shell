#include "daemonize.h"
#include "logger.h" // For logging errors
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h> // For strerror

int daemonize_process() {
    pid_t pid;

    // Fork off the parent process
    pid = fork();
    if (pid < 0) {
        log_perror("daemonize: fork #1 failed");
        return -1; // Fork failure
    }
    if (pid > 0) {
        // Parent process: exit successfully
        log_debug("daemonize: Parent exiting after first fork.");
        exit(EXIT_SUCCESS);
    }

    // Child process continues (becomes session leader)
    log_debug("daemonize: Child #1 (PID %d) continuing.", getpid());

    // Create a new session ID
    if (setsid() < 0) {
        log_perror("daemonize: setsid failed");
        return -1; // setsid failure
    }
    log_debug("daemonize: New session created (PID %d).", getpid());

    // Fork again
    pid = fork();
    if (pid < 0) {
        log_perror("daemonize: fork #2 failed");
        return -1; // Fork failure
    }
    if (pid > 0) {
        // Intermediate child process: exit successfully
        log_debug("daemonize: Intermediate child exiting after second fork.");
        exit(EXIT_SUCCESS);
    }

    // Grandchild process continues (the actual daemon)
    log_info("Daemon process started (PID %d).", getpid());


    // Close standard file descriptors
    log_debug("daemonize: Closing standard file descriptors.");
    if (close(STDIN_FILENO) < 0) {
        log_perror("daemonize: close(STDIN_FILENO) failed");
        // Continue if closing fails
    }
    if (close(STDOUT_FILENO) < 0) {
        log_perror("daemonize: close(STDOUT_FILENO) failed");
        // Continue if closing fails
    }
     // Close stderr *last* so we can still log errors from closing stdin/stdout
    if (close(STDERR_FILENO) < 0) {
        // Cannot log this error easily as stderr is now closed (or failed to close)
        // We could try reopening stderr to /dev/null first, then closing original.
        // For simplicity, just attempt close.
    }

    // Redirect standard file descriptors to /dev/null
    // Note: Logger might be using stderr, redirecting it might affect logging
    // unless the logger was initialized *after* daemonization or explicitly
    // handles its own file descriptor. Assuming logger uses its own fd if configured.
    log_debug("daemonize: Redirecting stdio to /dev/null.");
    int fd0 = open("/dev/null", O_RDWR); // stdin
    int fd1 = open("/dev/null", O_RDWR); // stdout
    int fd2 = open("/dev/null", O_RDWR); // stderr

    if (fd0 == -1 || fd1 == -1 || fd2 == -1) {
        // Log error if possible (might fail if stderr redirection failed)
        log_error("daemonize: Failed to open /dev/null for stdio redirection: %s", strerror(errno));
        // Clean up any successfully opened ones
        if (fd0 != -1) close(fd0);
        if (fd1 != -1) close(fd1);
        if (fd2 != -1) close(fd2);
        return -1; // Indicate failure
    }

    // Redirect stdin, stdout, stderr if opening /dev/null succeeded
    if (dup2(fd0, STDIN_FILENO) < 0) log_perror("daemonize: dup2(stdin) failed");
    if (dup2(fd1, STDOUT_FILENO) < 0) log_perror("daemonize: dup2(stdout) failed");
    if (dup2(fd2, STDERR_FILENO) < 0) log_perror("daemonize: dup2(stderr) failed");

    // Close the original /dev/null descriptors if they are not 0, 1, 2
    if (fd0 > 2) close(fd0);
    if (fd1 > 2) close(fd1);
    if (fd2 > 2) close(fd2);

    log_debug("daemonize: Stdio redirected.");

    return 0; // Success
}