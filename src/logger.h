#ifndef LOGGER_H
#define LOGGER_H
#include <stdarg.h>

// Inicializuje log: ak path!=NULL, zapisuje do súboru, inak na stderr.
// Verbose režim určuje, či sa vypisujú DEBUG správy.
int  log_init(const char *path, int verbose); // Use const char *
void log_info(const char *fmt, ...);          // Use const char *
void log_error(const char *fmt, ...);         // Use const char *
void log_perror(const char *msg);             // Use const char *
void log_debug(const char *fmt, ...);         // Use const char *
void log_close(void);
#endif // LOGGER_H