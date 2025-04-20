#ifndef LOGGER_H
#define LOGGER_H
#include <stdarg.h>

// Initializes the logger: if path != NULL, writes to the file, does not log
// Verbose mode determines whether DEBUG messages are printed. If we are logging to
// a file, we will log DEBUG messages to the file as well. Default is stderr.
int  log_init(const char *path, int verbose); // Use const char *
void log_info(const char *fmt, ...);          // Use const char *
void log_error(const char *fmt, ...);         // Use const char *
void log_perror(const char *msg);             // Use const char *
void log_debug(const char *fmt, ...);         // Use const char *
void log_close(void);
#endif // LOGGER_H