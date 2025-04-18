#include "logger.h"
#include <stdio.h>      // For snprintf, vsnprintf, stderr, perror
#include <stdlib.h>
#include <time.h>
#include <unistd.h>     // For write, close, STDERR_FILENO
#include <fcntl.h>      // For open, O_WRONLY, O_CREAT, O_APPEND
#include <string.h>     // For strlen, strerror
#include <stdarg.h>     // For va_list, va_start, va_end
#include <errno.h>      // For errno

#define LOG_BUFFER_SIZE 1024 // Max size for a single log message line

static int log_fd = STDERR_FILENO; // Default to stderr file descriptor
static int is_verbose = 0;

// Helper function to handle write errors and ensure all bytes are written
static int write_all(int fd,char *buf, size_t len) {
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t written = write(fd, buf + total_written, len - total_written);
        if (written < 0) {
            // EAGAIN/EWOULDBLOCK might happen on non-blocking sockets, but less likely for files.
            // EINTR is recoverable.
            if (errno == EINTR) {
                continue;
            }
            // Report error to original stderr in case log_fd is the one failing
            // Use fprintf + strerror instead of perror for more control over output fd
            fprintf(stderr, "ERROR: Failed to write to log descriptor %d: %s\n", fd, strerror(errno));
            return -1; // Indicate failure
        }
        total_written += (size_t)written;
    }
    return 0; // Indicate success
}


int log_init(char *path, int verbose) {
    is_verbose = verbose;
    int new_fd = -1;

    // Check if path is not NULL and not an empty string
    if (path && path[0] != '\0') {
        // Open the file for writing only, create if it doesn't exist, append to the end.
        // Set permissions to rw-r--r-- (0644).
        new_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (new_fd < 0) {
            // Use perror to print the system error message to stderr.
            perror("ERROR: cannot open log file");
            // Optionally add the path to the message using fprintf
            fprintf(stderr, "ERROR: cannot open log file '%s': %s\n", path, strerror(errno));
            return -1; // Return error
        }else{
            printf("INFO: Logging to file '%s'\n", path);
        }
    } else {
        new_fd = STDERR_FILENO;
    }

    // If a previous file was opened (and it wasn't stderr), close it first.
    if (log_fd != STDERR_FILENO && log_fd >= 0) {
        if (close(log_fd) < 0) {
             // Report close error to original stderr
             perror("ERROR: Failed to close previous log descriptor");
             // Continue, but report the issue. The old fd might be leaked if open failed.
        }
    }

    log_fd = new_fd; // Update the global log file descriptor

    return 0; // Return success
}

static void log_prefix(char *level) {
    time_t t = time(NULL);
    struct tm tm_info;
    char time_buf[20]; // Buffer for "YYYY-MM-DD HH:MM:SS"
    char prefix_buf[64]; // Buffer for full prefix "YYYY-MM-DD HH:MM:SS [LEVEL] "

    // Use localtime_r for thread-safety if threading is a concern
    if (localtime_r(&t, &tm_info) == NULL) {
         // Handle potential error from localtime_r
         char *fallback = "[TIME_ERR] ";
         write_all(log_fd, fallback, strlen(fallback));
         return;
    }

    if (strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info) == 0) {
        // Handle potential error from strftime
        char *fallback = "[STRFTIME_ERR] ";
        write_all(log_fd, fallback, strlen(fallback));
        return;
    }


    // Format the full prefix string
    int len = snprintf(prefix_buf, sizeof(prefix_buf), "%s [%s] ", time_buf, level);
    if (len < 0 || (size_t)len >= sizeof(prefix_buf)) {
        // Handle snprintf error or truncation (though unlikely with this size)
        char *fallback = "[PREFIX_ERR] ";
        write_all(log_fd, fallback, strlen(fallback));
        return;
    }

    write_all(log_fd, prefix_buf, (size_t)len);
}

// Generic log function using varargs
static void log_generic(char *level, char *fmt, va_list ap) {
    char msg_buf[LOG_BUFFER_SIZE];

    log_prefix(level); // Write the prefix first

    // Format the user message using vsnprintf for safety
    int msg_len = vsnprintf(msg_buf, sizeof(msg_buf) - 1, fmt, ap); // Leave space for newline
    if (msg_len < 0) {
        // Handle vsnprintf encoding error
        char *fallback = "Failed to format log message.\n";
        write_all(log_fd, fallback, strlen(fallback));
        return;
    }

    // Ensure buffer is not overflowed (vsnprintf returns needed size)
    size_t write_len = (size_t)msg_len < sizeof(msg_buf) - 1 ? (size_t)msg_len : sizeof(msg_buf) - 2;

    // Write the formatted message
    if (write_all(log_fd, msg_buf, write_len) == 0) {
        // Write the newline character separately
        write_all(log_fd, "\n", 1);
    }
    // write_all already prints errors to stderr if it fails
}


void log_info(char *fmt, ...) {  // Log info message
    va_list ap;  // Initialize variable argument list
    va_start(ap, fmt);  // This is the first argument after the fixed arguments
    log_generic("INFO", fmt, ap);  // Call the generic log function
    va_end(ap);  // Clean up the variable argument list
}

// Log error message
void log_error(char *fmt, ...) {  // Log error message
    va_list ap;  // Initialize variable argument list
    va_start(ap, fmt);  // This is the first argument after the fixed arguments
    log_generic("ERROR", fmt, ap);  // Call the generic log function
    va_end(ap);  // Clean up the variable argument list
}

// Log error including the system error message for errno
void log_perror(char *msg) {
    // Get the error message before doing anything else that might change errno
    char *err_str = strerror(errno);
    char combined_msg[LOG_BUFFER_SIZE];

    log_prefix("ERROR"); // Write the prefix

    // Format the user message + system error message
    int len = snprintf(combined_msg, sizeof(combined_msg) -1, "%s: %s", msg, err_str);
    if (len < 0) {
        // Handle snprintf error
        char *fallback = "Failed to format perror message.\n";
        write_all(log_fd, fallback, strlen(fallback));
        return;
    }

    size_t write_len = (size_t)len < sizeof(combined_msg) - 1 ? (size_t)len : sizeof(combined_msg) - 2;

    // Write the combined message
    if (write_all(log_fd, combined_msg, write_len) == 0) {
        // Write the newline character separately
        write_all(log_fd, "\n", 1);
    }
}


void log_debug(char *fmt, ...) {
    if (!is_verbose) return;

    va_list ap;  // Initialize variable argument list

    // Use va_start to initialize the variable argument list
    va_start(ap, fmt);  // This is the first argument after the fixed arguments

    // Temporarily switch to stderr for debug messages
    int original_log_fd = log_fd;  // Save the current log fd
    log_fd = STDERR_FILENO;  // Set log fd to stderr

    log_generic("DEBUG", fmt, ap);  // Call the generic log function

    // Restore original log fd
    log_fd = original_log_fd;  // Set log fd back to the original value

    va_end(ap);  // Clean up the variable argument list
}

void log_close(void) {
    // Only close if it's a valid file descriptor and not stderr
    if (log_fd != STDERR_FILENO && log_fd >= 0) {
        if (close(log_fd) < 0) {
            // Report close error to original stderr using perror
            perror("ERROR: Failed to close log descriptor");
        }
        log_fd = -1; // Mark as closed/invalid
    }
    // Reset to default in case log_init is called again later
    log_fd = STDERR_FILENO;
    is_verbose = 0;
}