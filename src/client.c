#include "client.h"
#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // For read, write, close, STDIN_FILENO, STDOUT_FILENO, shutdown
#include <errno.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>

#define BUFFER_SIZE 4096 // Use a larger buffer

// Helper function to write all data, handling partial writes and EINTR
static int write_all(int fd, const char *buf, size_t len) {
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t written = write(fd, buf + total_written, len - total_written);
        if (written < 0) {
            if (errno == EINTR) continue; // Interrupted system call, try again
            log_perror("write failed");   // Log other errors
            return -1; // Indicate failure
        }
        if (written == 0) {
            // This shouldn't typically happen for blocking sockets/pipes unless fd is closed.
            log_error("write returned 0 unexpectedly (fd %d)", fd);
            return -1; // Indicate failure
        }
        total_written += (size_t)written;
    }
    return 0; // Indicate success
}


/**
 * @brief Connects to the server, forwards stdin to the server,
 *        and prints server responses to stdout. Exits when
 *        the server closes the connection or on EOF from stdin.
 *
 * @param cfg Pointer to the Config struct (port and bind_addr).
 * @return 0 on normal exit, -1 on error.
 */
int client_run(Config *cfg) {
    int sockfd;
    struct sockaddr_in serv_addr;

    // Create TCP socket (Still IPv4 only based on this structure)
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        log_perror("socket");
        return -1;
    }

    // Prepare server address structure
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(cfg->port);
    // Using bind_addr as target host - consider renaming in Config for clarity
    const char *addr = cfg->bind_addr[0] ? cfg->bind_addr : "127.0.0.1";
    if (inet_pton(AF_INET, addr, &serv_addr.sin_addr) <= 0) {
        log_error("Invalid address: %s", addr);
        close(sockfd);
        return -1;
    }

    // Connect to server
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        log_perror("connect");
        close(sockfd);
        return -1;
    }
    log_info("Connected to %s:%d", addr, cfg->port);

    // Main loop: multiplex stdin and socket using select()
    fd_set readfds;
    int maxfd;
    char buf[BUFFER_SIZE]; // Use defined buffer size
    int stdin_open = 1; // Flag to track if stdin is still open

    while (1) {
        FD_ZERO(&readfds);
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &readfds); // Monitor stdin only if it's open
        }
        FD_SET(sockfd, &readfds);

        // Determine maxfd for select
        maxfd = sockfd;
        if (stdin_open && STDIN_FILENO > maxfd) {
            maxfd = STDIN_FILENO;
        }

        int ready = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue; // Interrupted, restart select
            log_perror("select");
            break; // Exit on other select errors
        }

        // Data from server
        if (FD_ISSET(sockfd, &readfds)) {
            ssize_t n = read(sockfd, buf, sizeof(buf));
            if (n < 0) {
                log_perror("read from server");
                break; // Exit on read error
            } else if (n == 0) {
                log_info("Server closed connection");
                break; // Exit loop, server disconnected
            }
            // Write received data to stdout, handling partial writes
            if (write_all(STDOUT_FILENO, buf, n) < 0) {
                log_error("Failed to write to stdout.");
                break; // Exit on stdout write error
            }
        }

        // Input from user (stdin)
        if (stdin_open && FD_ISSET(STDIN_FILENO, &readfds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf)); // Use read instead of fgets
            if (n < 0) {
                log_perror("read from stdin");
                break; // Exit on stdin read error
            } else if (n == 0) {
                // EOF or error on stdin
                log_info("EOF on stdin, shutting down write connection.");
                stdin_open = 0; // Stop monitoring stdin
                // Gracefully close the write half of the connection
                if (shutdown(sockfd, SHUT_WR) < 0) {
                    log_perror("shutdown(SHUT_WR) failed");
                    // Don't break here, allow receiving remaining server data
                }
                // No FD_CLR needed here as we check stdin_open before FD_SET
            } else {
                // Send data read from stdin to the server, handling partial writes
                if (write_all(sockfd, buf, n) < 0) {
                    log_error("Failed to write to server.");
                    break; // Exit on server write error
                }
            }
        }
    } // end while(1)

    log_info("Client loop finished. Closing socket.");
    close(sockfd);
    return 0;
}