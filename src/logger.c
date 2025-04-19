#include "logger.h"
#include <stdio.h>      // For snprintf, vsnprintf, stderr, perror, dprintf
#include <stdlib.h>
#include <time.h>
#include <unistd.h>     // For write, close, STDERR_FILENO, getpid
#include <fcntl.h>      // For open, O_WRONLY, O_CREAT, O_APPEND
#include <string.h>     // For strlen, strerror
#include <stdarg.h>     // For va_list, va_start, va_end
#include <errno.h>      // For errno

#define LOG_BUFFER_SIZE 1024 // Max size for a single log message line

// Use STDERR_FILENO as the initial default
static int log_fd = STDERR_FILENO;
static int is_verbose = 0;

// Helper function to handle write errors and ensure all bytes are written
static int write_all(int fd, const char *buf, size_t len) {
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t written = write(fd, buf + total_written, len - total_written);
        if (written < 0) {
            // EINTR is recoverable.
            if (errno == EINTR) {
                continue;
            }
            // Report error to original stderr in case log_fd is the one failing
            // Use dprintf + strerror for more control over output fd
            dprintf(STDERR_FILENO, "ERROR: Failed to write to log descriptor %d: %s\n", fd, strerror(errno));
            return -1; // Indicate failure
        }
        total_written += (size_t)written;
    }
    return 0; // Indicate success
}


int log_init(const char *path, int verbose) {
    is_verbose = verbose;
    int new_fd = STDERR_FILENO; // Default to stderr

    // Check if path is not NULL and not an empty string
    if (path && path[0] != '\0') {
        // Open the file for writing only, create if it doesn't exist, append to the end.
        // Set permissions to rw-r--r-- (0644).
        new_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (new_fd < 0) {
            // Report error to stderr.
            dprintf(STDERR_FILENO, "ERROR: cannot open log file '%s': %s\n", path, strerror(errno));
            // Keep new_fd as STDERR_FILENO (fallback)
            new_fd = STDERR_FILENO;
            // Return error to indicate the requested file wasn't opened
            // return -1; // Decide if this should be a fatal error for the caller
        } else {
            // Use dprintf to stderr to report success
            dprintf(STDERR_FILENO, "INFO: Logging to file '%s' (fd %d)\n", path, new_fd);
        }
    }

    // If the current log_fd is a file (not stderr), close it.
    if (log_fd != STDERR_FILENO && log_fd >= 0) {
        if (close(log_fd) < 0) {
             // Report close error to original stderr
             dprintf(STDERR_FILENO, "ERROR: Failed to close previous log descriptor %d: %s\n", log_fd, strerror(errno));
             // Continue, but report the issue.
        }
    }

    // Update the global log file descriptor
    log_fd = new_fd;

    // Log initialization message using the new log_fd
    log_info("Logger initialized. Verbose: %s.", verbose ? "enabled" : "disabled");

    return 0; // Return success (even if fallback to stderr occurred)
}

// Generic log function using varargs - ensures atomic write
static void log_generic(const char *level, const char *fmt, va_list ap) {
    // Buffer to hold the complete formatted log message
    char log_buffer[LOG_BUFFER_SIZE];
    char time_buf[20]; // Buffer for "YYYY-MM-DD HH:MM:SS"
    time_t t = time(NULL);
    struct tm tm_info;

    // Use localtime_r for thread-safety
    if (localtime_r(&t, &tm_info) == NULL) {
         // Handle potential error from localtime_r
         snprintf(time_buf, sizeof(time_buf), "[TIME_ERR]");
    } else if (strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info) == 0) {
        // Handle potential error from strftime
        snprintf(time_buf, sizeof(time_buf), "[STRFTIME_ERR]");
    }

    // Start formatting the message into the buffer (Timestamp, Level, PID)
    int current_len = snprintf(log_buffer, sizeof(log_buffer), "%s [%s] (pid %d) ",
                               time_buf, level, getpid());

    // Append the user message if space allows
    if (current_len > 0 && (size_t)current_len < sizeof(log_buffer)) {
        int remaining_space = sizeof(log_buffer) - current_len;
        int msg_len = vsnprintf(log_buffer + current_len, remaining_space, fmt, ap);

        if (msg_len < 0) {
            // Handle vsnprintf encoding error
            // Overwrite the user message part with an error indicator
            snprintf(log_buffer + current_len, remaining_space, "[FORMAT_ERR]");
            current_len = strlen(log_buffer); // Recalculate length
        } else {
            current_len += msg_len;
        }
    }

    // Ensure buffer is null-terminated and add newline, handling potential truncation
    if (current_len < 0) current_len = 0; // Should not happen, but safety first

    if ((size_t)current_len >= sizeof(log_buffer) - 1) {
        // Message (or prefix+message) was too long and got truncated
        current_len = sizeof(log_buffer) - 2; // Make space for newline and null terminator
        log_buffer[current_len] = '\n';       // Add newline
        log_buffer[current_len + 1] = '\0';   // Add null terminator
    } else {
        // Message fit, just add newline and null terminator
        log_buffer[current_len] = '\n';
        log_buffer[current_len + 1] = '\0';
        current_len++; // Include newline in length for write_all
    }

    // Write the complete buffer in a single call for atomicity
    write_all(log_fd, log_buffer, (size_t)current_len);
}


void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_generic("INFO", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_generic("ERROR", fmt, ap);
    va_end(ap);
}

// Log error including the system error message for errno
void log_perror(const char *msg) {
    // Get the error message before doing anything else that might change errno
    char *err_str = strerror(errno);
    // Use log_error to format the combined message
    log_error("%s: %s", msg, err_str);
}


void log_debug(const char *fmt, ...) {
    if (!is_verbose) return;

    va_list ap;
    va_start(ap, fmt);
    log_generic("DEBUG", fmt, ap);
    va_end(ap);
}

void log_close(void) {
    log_info("Closing logger."); // Log closing message before actually closing
    // Only close if it's a valid file descriptor and not stderr
    if (log_fd != STDERR_FILENO && log_fd >= 0) {
        if (close(log_fd) < 0) {
            // Report close error to original stderr using dprintf
            dprintf(STDERR_FILENO, "ERROR: Failed to close log descriptor %d: %s\n", log_fd, strerror(errno));
        }
    }
    // Reset to default state
    log_fd = STDERR_FILENO;
    is_verbose = 0;
}