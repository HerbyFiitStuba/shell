#ifndef LOGGER_H
#define LOGGER_H
#include <stdarg.h>

// Inicializuje log: ak path!=NULL, zapisuje do súboru, inak na stderr.
// Verbose režim určuje, či sa vypisujú DEBUG správy.
int  log_init(char *path, int verbose);
void log_info(char *fmt, ...);
void log_error(char *fmt, ...);
void log_debug(char *fmt, ...);
void log_close(void);
#endif // LOGGER_H