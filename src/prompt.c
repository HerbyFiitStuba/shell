#include "prompt.h"
#include <stdio.h>      // For snprintf
#include <time.h>       // For time, localtime_r, strftime
#include <unistd.h>     // For gethostname, geteuid
#include <pwd.h>        // For getpwuid
#include <string.h>     // For strncpy
#include <limits.h>     // For HOST_NAME_MAX (or use POSIX version like _POSIX_HOST_NAME_MAX)
#include <sys/types.h>  // For uid_t

// Define HOST_NAME_MAX if not defined (e.g., older POSIX)
#ifndef HOST_NAME_MAX
#ifdef _POSIX_HOST_NAME_MAX
#define HOST_NAME_MAX _POSIX_HOST_NAME_MAX
#else
#define HOST_NAME_MAX 255 // A common fallback value
#endif
#endif


// Generate the prompt string: HH:MM user@host#
const char* generate_prompt(void) {
    static char prompt_buf[256]; // Static buffer to hold the prompt
    char time_buf[6];            // HH:MM + null terminator
    char hostname[HOST_NAME_MAX + 1];  // Buffer for hostname
    char *username = NULL;       // Pointer for username
    time_t now;               // Current time
    struct tm tm_info;        // Structure to hold local time
    struct passwd *pw;        // Pointer for user information
    uid_t uid;

    // Get current time
    now = time(NULL);
    if (localtime_r(&now, &tm_info) == NULL) {
        // Handle error, maybe return a default prompt
        snprintf(prompt_buf, sizeof(prompt_buf), "[TIME_ERR]# ");
        return prompt_buf;
    }
    // Format time as HH:MM
    if (strftime(time_buf, sizeof(time_buf), "%H:%M", &tm_info) == 0) { 
        // Handle error
        snprintf(prompt_buf, sizeof(prompt_buf), "[STRFTIME_ERR]# ");
        return prompt_buf;
    }

    // Get username
    uid = geteuid();     // Get effective user ID
    pw = getpwuid(uid);  // Get user information
    if (pw) {
        username = pw->pw_name;  // Use username from passwd struct
    } else {
        // Handle error or fallback to UID
        static char uid_str[16]; // Static buffer for UID string
        snprintf(uid_str, sizeof(uid_str), "%d", uid);
        username = uid_str;
    }

    // Get hostname
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        // Handle error
        strncpy(hostname, "unknown", sizeof(hostname) - 1);
        hostname[sizeof(hostname) - 1] = '\0'; // Ensure null termination
    } else {
        // Ensure null termination even on success if buffer was full
        hostname[sizeof(hostname) - 1] = '\0';
    }


    // Combine into the final prompt string
    snprintf(prompt_buf, sizeof(prompt_buf), "%s %s@%s# ", time_buf, username, hostname);

    return prompt_buf;
}