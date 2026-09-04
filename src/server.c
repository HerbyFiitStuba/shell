#include "server.h"
#include "logger.h"
#include "prompt.h" 
#include "executor.h" 
#include "parser.h" 
#include "tokenizer.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>     // Needed for malloc, realloc, free
#include <sys/epoll.h>  // Added for epoll
#include <fcntl.h>      // Added for fcntl 
#include <signal.h>     // Added for sig_atomic_t
#include <stdio.h>      // For snprintf
#include <time.h>       // For time()
#include <limits.h> // For strtol validation

#define WRITE_BUFFER_SIZE 512 // Buffer for formatting messages before write_all

// Initial size and growth step for the dynamic buffer
static const size_t INITIAL_BUFFER_SIZE = 128; // Initial size of the buffer
static const size_t GROWTH_INCREMENT = 128; // Growth step for the buffer
static const size_t MAX_EVENTS = 64;       // Max events per epoll_wait call

// --- Global flag to signal server halt ---
static volatile sig_atomic_t halt_requested = 0;

// --- Global Client Management (Dynamic Array) ---
static client_state **clients = NULL; // Pointer to array of client_state pointers
static int client_count = 0;          // Number of active clients
static int client_capacity = 0;       // Allocated capacity of the clients array
#define CLIENT_ARRAY_GROWTH 16        // How many slots to add when resizing
// #define CLIENT_TIMEOUT_SECONDS (10 * 60) // Removed - now comes from cfg
#define EPOLL_TIMEOUT_MS (30 * 1000)     // Check timeouts every 30 seconds

// --- Helper Functions ---

// Make a socket non-blocking
static int make_socket_non_blocking(int sfd) {
    int flags = fcntl(sfd, F_GETFL, 0);  // Get current flags
    if (flags == -1) {
        log_perror("fcntl(F_GETFL) failed");
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sfd, F_SETFL, flags) == -1) {
        log_perror("fcntl(F_SETFL) failed");
        return -1;
    }
    return 0;
}

// Find the index in the dynamic clients array for a given fd
static int find_client_index(int fd) {
    for (int i = 0; i < client_count; ++i) { // Use client_count
        if (clients[i] && clients[i]->fd == fd) {
            return i;
        }
    }
    return -1; // Not found
}

// Add a new client state to the dynamic array
static int add_client(int fd) {
    // Check if resize needed
    if (client_count == client_capacity) {
        int new_capacity = client_capacity + CLIENT_ARRAY_GROWTH;
        client_state **new_clients = realloc(clients, new_capacity * sizeof(client_state*));
        if (!new_clients) {
            log_error("Failed to realloc client array to capacity %d", new_capacity);
            // Cannot add client, refuse connection or handle error
            return -1;
        }
        clients = new_clients;
        client_capacity = new_capacity;
        log_debug("Resized client array capacity to %d", client_capacity);
    }

    // Allocate client state struct
    client_state *new_state = malloc(sizeof(client_state));
    if (!new_state) {
         log_error("Failed to allocate memory for client state (fd: %d)", fd);
         return -1;
    }
    // Initialize new_state
    new_state->fd = fd;
    new_state->buffer = NULL;
    new_state->buffer_size = 0;
    new_state->buffer_capacity = 0;
    new_state->last_activity = time(NULL); // Initialize last activity time

    // Add to array
    clients[client_count] = new_state;
    int new_index = client_count;
    client_count++;

    log_debug("Added client state for fd %d at index %d (count=%d)", fd, new_index, client_count);
    return new_index; // Return the index where added
}

// Remove and clean up a client state by fd from the dynamic array
static void remove_client(int fd, int epoll_fd) {
    int index = find_client_index(fd); // Uses client_count internally now
    if (index != -1) {
        log_debug("Removing client fd %d (index %d, count before=%d)", fd, index, client_count);

        // 1. Remove from epoll
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
            log_perror("epoll_ctl(DEL) failed");
        }
        // 2. Close socket
        close(fd);
        // 3. Free client-specific buffer
        free(clients[index]->buffer);
        // 4. Free the state struct itself
        free(clients[index]);

        // 5. Shift last element into the gap (if not removing the last element)
        int last_index = client_count - 1;
        if (index < last_index) {
            clients[index] = clients[last_index];
            log_debug("Moved client from index %d to %d", last_index, index);
        }
        // Set the now-unused last slot to NULL (optional but good practice)
        clients[last_index] = NULL;

        // 6. Decrement count
        client_count--;
        log_debug("Client count after removal: %d", client_count);

    } else {
        log_error("Attempted to remove non-existent client fd %d", fd);
        // Check if fd is valid before closing
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            close(fd); // Close fd just in case
        }
    }

    log_debug("Client fd %d has been removed", fd);

}

// Helper function to write all data, handling partial writes and EINTR
static int write_all(int fd, const char *buf, size_t len) {
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t written = write(fd, buf + total_written, len - total_written);
        if (written < 0) {
            if (errno == EINTR) continue; // Interrupted system call, try again
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "ERROR: write failed in write_all (fd=%d): %s\n", fd, strerror(errno));
            write(STDERR_FILENO, err_buf, strlen(err_buf)); // Use raw write to stderr
            return -1; // Indicate failure
        }
        if (written == 0 && len > 0) {
            // This shouldn't typically happen.
            char err_buf[128];
            snprintf(err_buf, sizeof(err_buf), "ERROR: write returned 0 unexpectedly (fd=%d)\n", fd);
            write(STDERR_FILENO, err_buf, strlen(err_buf));
            return -1; // Indicate failure
        }
        total_written += (size_t)written;
    }
    return 0; // Indicate success
}

// Handle data received from a client
static void handle_client_read(int fd, int epoll_fd, const Config *cfg) {
    int index = find_client_index(fd);
    if (index == -1) {
        log_error("Read event for unknown client fd %d", fd);
        remove_client(fd, epoll_fd); // Clean up this unknown fd
        return;
    }

    client_state *client = clients[index];
    char read_buf[512]; // Temporary buffer for reading chunks
    int data_read_this_call = 0; // Flag to check if any data was actually read

    log_debug("Handling read for fd %d", fd);

    while (1) {
        ssize_t count = read(fd, read_buf, sizeof(read_buf));
        if (count == -1) {
            // If errno is EAGAIN or EWOULDBLOCK, it means we have read all data for now
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                log_debug("Finished reading for fd %d (EAGAIN/EWOULDBLOCK)", fd);
                break;
            }
            // Other errors are fatal for this connection
            log_perror("read failed");
            remove_client(fd, epoll_fd);
            return;
        } else if (count == 0) {
            // End of file. The remote has closed the connection.
            log_info("Client fd %d disconnected (EOF)", fd);
            remove_client(fd, epoll_fd);
            return;
        }

        // Mark that data was read
        data_read_this_call = 1;

        // Append read data to client's dynamic buffer
        size_t required_capacity = client->buffer_size + count;
        if (required_capacity > client->buffer_capacity) {
            size_t new_capacity = client->buffer_capacity;
            if (new_capacity == 0) {
                new_capacity = INITIAL_BUFFER_SIZE;
            }
            while (new_capacity < required_capacity) {
                new_capacity += GROWTH_INCREMENT;
            }

            char *new_buffer = realloc(client->buffer, new_capacity);
            if (!new_buffer) {
                log_error("realloc failed for fd %d: %s", fd, strerror(errno));
                remove_client(fd, epoll_fd); // Cannot continue without buffer
                return;
            }
            client->buffer = new_buffer;
            client->buffer_capacity = new_capacity;
            log_debug("Resized buffer for fd %d to %zu bytes", fd, new_capacity);
        }
        memcpy(client->buffer + client->buffer_size, read_buf, count);
        client->buffer_size += count;
    }

    // Update last activity time *only if* data was actually read in this call
    if (data_read_this_call) {
        client->last_activity = time(NULL);
        log_debug("Updated last_activity for fd %d", fd);
    }

    // Process complete lines from the buffer
    char *line_start = client->buffer;
    char *newline_pos;
    char msg_buf[WRITE_BUFFER_SIZE]; // Buffer for messages
    int msg_len;

    while ((newline_pos = memchr(line_start, '\n', client->buffer_size - (line_start - client->buffer))) != NULL) {
        // size_t line_len = newline_pos - line_start;

        // Temporarily null-terminate the line for processing
        *newline_pos = '\0';
        
        // --- Replace echo with parsing and execution ---
        log_debug("Received line from fd %d: %s", fd, line_start);
        
        Token *tokens = NULL;
        Command *cmd_sequence = NULL;
        int exec_result = EXEC_OK; // Default result

        // 1. Tokenize
        tokens = tokenize(line_start);
        if (tokens == NULL) {
            log_error("Tokenizer failed for input from fd %d", fd);
            msg_len = snprintf(msg_buf, sizeof(msg_buf), "Error: Tokenization failed.\n");
            if (msg_len > 0) write_all(fd, msg_buf, msg_len);
            exec_result = EXEC_ERROR; // Mark as error to skip further processing this line
        } else {
            // 2. Parse
            cmd_sequence = parse(tokens);
            if (cmd_sequence == NULL) {
                log_error("Parser failed for input from fd %d", fd);
                msg_len = snprintf(msg_buf, sizeof(msg_buf), "Error: Command parsing failed (syntax error?).\n");
                if (msg_len > 0) write_all(fd, msg_buf, msg_len);
                exec_result = EXEC_ERROR; // Mark as error
            } else {
                // 3. Execute
                log_debug("Executing command sequence for fd %d", fd);
                exec_result = execute_command_sequence(cmd_sequence, fd, cfg, clients, client_count);
                log_debug("Execution finished for fd %d with result %d", fd, exec_result);
            }
        }

        // Handle execution results
        if (exec_result == EXEC_QUIT) {
            log_info("Client fd %d requested quit.", fd);
            remove_client(fd, epoll_fd); // Close connection
            free_command_sequence(cmd_sequence); // Free parsed structure
            free_tokens(tokens);                 // Free tokens
            return; // Stop processing buffer for this client
        } else if (exec_result == EXEC_HALT) {
            log_info("Halt command received from fd %d. Initiating server shutdown.", fd);
            halt_requested = 1; // Set the global flag to signal shutdown
            msg_len = snprintf(msg_buf, sizeof(msg_buf), "Server halt initiated.\n"); // Inform client
            if (msg_len > 0) write_all(fd, msg_buf, msg_len);
            // The main loop will detect the flag and break
        } else if (exec_result == EXEC_ABORT_N) {
            // Executor already validated the command and arguments
            char *endptr;
            errno = 0;
            long target_index_long = strtol(cmd_sequence->argv[1], &endptr, 10);
            // Basic check again in case of race conditions or memory issues
            if (errno == 0 && endptr != cmd_sequence->argv[1] && *endptr == '\0' && target_index_long >= 0 && target_index_long < client_count && target_index_long <= INT_MAX) {
                int target_index = (int)target_index_long;
                if (clients[target_index] != NULL) { // Check if target client still exists
                    int target_fd = clients[target_index]->fd;
                    log_info("Client fd %d requested abort for client index %d (fd %d).", fd, target_index, target_fd);

                    // Inform the target client 
                    msg_len = snprintf(msg_buf, sizeof(msg_buf), "Connection aborted by server (abort command).\n");
                    if (msg_len > 0) {
                        write_all(target_fd, msg_buf, msg_len); // Ignore error
                    }

                    // Remove the target client
                    remove_client(target_fd, epoll_fd);

                    // Check if the current client aborted itself
                    if (target_fd == fd) {
                        log_info("Client fd %d aborted itself.", fd);
                        free_command_sequence(cmd_sequence); // Free parsed structure
                        free_tokens(tokens);                 // Free tokens
                        return; // Stop processing, current client state is gone
                    }
                } else {
                    log_error("Client fd %d tried to abort client index %d, but client no longer exists.", fd, target_index);
                    msg_len = snprintf(msg_buf, sizeof(msg_buf), "Error: Client with index %d no longer exists.\n", target_index);
                    if (msg_len > 0) write_all(fd, msg_buf, msg_len);
                }
            } else {
                // Should not happen if executor validation is correct, but log defensively
                log_error("Internal error: Invalid index '%s' received from executor for abort command from fd %d.", cmd_sequence->argv[1], fd);
            }
        } else if (exec_result == EXEC_ERROR) {
            // This now indicates a critical error during pipeline setup (fork, pipe etc.)
            // or a command terminated by signal. Simple command failures (exit != 0)
            // should result in EXEC_OK from execute_command_sequence.
            log_error("Critical execution error occurred for fd %d (e.g., fork/pipe failure, signal).", fd);
            // Optionally inform the client, though stderr might have already received info
            msg_len = snprintf(msg_buf, sizeof(msg_buf), "Error: Critical internal error during command execution.\n");
            if (msg_len > 0) write_all(fd, msg_buf, msg_len);
            // Continue processing other lines for this client unless the error was fatal for the connection
        } else {
             // Includes EXEC_OK and potentially other values if execute_command_sequence is changed later.
             // EXEC_OK means the sequence completed structurally, even if individual commands failed.
             log_debug("Command sequence processed for fd %d with result code %d (EXEC_OK means sequence structure ok).", fd, exec_result);
             // Continue normally
        }


        // Free structures for the processed command/line
        free_command_sequence(cmd_sequence);
        free_tokens(tokens);

        // Advance line_start past the processed line (including the null terminator)
        line_start = newline_pos + 1;

        // --- Send prompt after processing command (only if client wasn't removed) ---
        // Check if the client still exists (it might have quit or aborted itself)
        if (find_client_index(fd) != -1) {
            const char *prompt = generate_prompt();
            ssize_t prompt_len = strlen(prompt);
            if (write_all(fd, prompt, prompt_len) < 0) {
                 if (errno == EPIPE) {
                     log_info("Client fd %d disconnected (Broken Pipe on prompt write)", fd);
                     remove_client(fd, epoll_fd);
                     return; // Client gone
                 }
                 // Other errors handled by write_all
            }
        } else {
            // Client was removed (quit/abort self), stop processing buffer for this fd
            log_debug("Client fd %d removed, stopping prompt send.", fd);
            return;
        }
        // --- End prompt sending ---

    }

    // Remove processed data from the buffer by shifting remaining data
    size_t remaining_size = client->buffer_size - (line_start - client->buffer);
    if (remaining_size > 0 && line_start != client->buffer) {
        memmove(client->buffer, line_start, remaining_size);
    }
    client->buffer_size = remaining_size;

}


// --- Main Server Logic ---

int socket_setup(const Config *cfg) {
    int listen_fd;
    struct sockaddr_in serv_addr;
    int opt = 1;

    // *** Create socket ***
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_perror("socket() failed");
        return -1;
    }

    // *** Set socket options ***
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_perror("setsockopt(SO_REUSEADDR) failed");
        close(listen_fd);
        return -1;
    }

    // *** Make listening socket non-blocking ***
    if (make_socket_non_blocking(listen_fd) == -1) {
        close(listen_fd);
        return -1;
    }

    // *** Bind to address and port ***
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;  // IPv4
    serv_addr.sin_port = htons(cfg->port);  // Port number in network byte order
    if (cfg->bind_addr[0] != '\0') {
        if (inet_pton(AF_INET, cfg->bind_addr, &serv_addr.sin_addr) <= 0) {
            log_error("inet_pton('%s') failed: %s", cfg->bind_addr, strerror(errno));
            close(listen_fd);
            return -1;
        }
    } else {
        serv_addr.sin_addr.s_addr = INADDR_ANY;
    }

    // *** Bind the socket to the address ***
    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {  // Bind to address
        log_perror("bind() failed");
        log_error("Failed to bind to %s:%d", cfg->bind_addr[0] ? cfg->bind_addr : "0.0.0.0", cfg->port);
        close(listen_fd);
        return -1;
    }

    // *** Set socket to listen ***
    if (listen(listen_fd, 128) < 0) { // Listen for incoming connections
        log_perror("listen() failed");
        close(listen_fd);
        return -1;
    }

    // *** Set socket to non-blocking mode ***
    log_info("Server listening non-blockingly on %s:%d",
             cfg->bind_addr[0] ? cfg->bind_addr : "0.0.0.0", cfg->port);
    return listen_fd;
}

// Main server loop
void server_run(const Config *cfg) { // Pass cfg as const pointer
    // Ensure globals are initialized
    clients = NULL;
    client_count = 0;
    client_capacity = 0;
    halt_requested = 0; // Ensure halt flag is reset at start

    int listen_fd = socket_setup(cfg);  // Setup socket
    if (listen_fd < 0) return;

    // --- Initialize epoll ---
    // epoll is used for efficient I/O event notification
    // epoll is more efficient than select/poll for large numbers of fds
    int epoll_fd = epoll_create1(0);  // Create epoll instance
    if (epoll_fd == -1) {
        log_perror("epoll_create1 failed");
        close(listen_fd);
        return;
    }

    struct epoll_event event;
    struct epoll_event events[MAX_EVENTS];

    event.data.fd = listen_fd;
    event.events = EPOLLIN | EPOLLET; // Listen for input, Edge Triggered
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
        log_perror("epoll_ctl(ADD listen_fd) failed");
        close(listen_fd);
        close(epoll_fd);
        return;
    }

    log_info("Starting epoll event loop...");

    // Main server loop
    while (!halt_requested) { // Loop until halt is requested

        // Use a timeout for epoll_wait to allow periodic checks
        int epoll_result = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);

        // Check halt flag *immediately* after epoll_wait returns,
        // especially if wait could be interrupted by signals in the future.
        if (halt_requested) {
            log_info("Halt flag detected after epoll_wait, breaking loop.");
            break;
        }

        if (epoll_result == -1) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, safe to continue
            }
            log_perror("epoll_wait failed");
            break; // Exit loop on other errors
        }

        // Process epoll events if any
        if (epoll_result > 0) {
            log_debug("epoll_wait returned %d events", epoll_result);
            for (int i = 0; i < epoll_result; i++) {  // Process each event
                uint32_t current_events = events[i].events;  // Get the event type
                int current_fd = events[i].data.fd;  // Get the fd associated with the event

                if ((current_events & EPOLLERR) || (current_events & EPOLLHUP)) {  // Check for error or hangup
                    // Error or hang up occurred on this fd.
                    log_error("epoll error/hangup on fd %d", current_fd);
                    // Check if it's the listening socket 
                    if (current_fd != listen_fd) {
                        remove_client(current_fd, epoll_fd); // Close and remove client
                    } else {
                        log_error("Error on listening socket %d! Shutting down?", current_fd);
                        goto cleanup; // Exit loop and clean up
                    }
                } else if (current_fd == listen_fd) {  // Check if it's the listening socket
                    // New incoming connection(s)
                    log_debug("Accepting new connections on listen_fd %d", listen_fd); 
                    while (1) {  // Accept all pending connections
                        struct sockaddr_in cli_addr;  // Client address structure
                        socklen_t cli_len = sizeof(cli_addr);  // Length of client address
                        int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);  // Accept connection

                        if (client_fd == -1) {  // Check for errors
                            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {  
                                // We have processed all incoming connections.
                                log_debug("Finished accepting connections (EAGAIN/EWOULDBLOCK)");
                                break;
                            } else {
                                log_perror("accept failed");
                                break; // Error accepting
                            }
                        }

                        char client_ip[INET_ADDRSTRLEN];  // Buffer for client IP address
                        inet_ntop(AF_INET, &cli_addr.sin_addr, client_ip, sizeof(client_ip));  // Convert to string
                        log_info("Client connected from %s:%d (fd: %d)", client_ip, ntohs(cli_addr.sin_port), client_fd);  // Log client info

                        if (make_socket_non_blocking(client_fd) == -1) {  // Make client socket non-blocking
                            close(client_fd);  // Close socket if we can't make it non-blocking
                            continue; // Failed to make non-blocking
                        }

                        int client_index = add_client(client_fd); // Use dynamic add_client
                        if (client_index == -1) {  // Check if client was added successfully
                            log_error("Failed to add client state for fd %d (capacity %d)", client_fd, client_capacity);
                            close(client_fd); // Close socket if we can't manage state
                            continue;
                        }

                        event.data.fd = client_fd;
                        // Monitor for input, edge-triggered, and remote close/half-close
                        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;  // EPOLLRDHUP for remote close
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {  // Add client to epoll
                            log_perror("epoll_ctl(ADD client_fd) failed");  // Log error
                            remove_client(client_fd, epoll_fd); // Clean up if add fails
                        } else {
                             log_debug("Added client fd %d to epoll", client_fd);
                             // --- Send initial prompt ---
                             const char *prompt = generate_prompt();
                             ssize_t prompt_len = strlen(prompt);
                             if (write_all(client_fd, prompt, prompt_len) < 0) {
                                 if (errno != EPIPE && errno != EAGAIN && errno != EWOULDBLOCK) {
                                    log_perror("initial prompt write failed");
                                    // Consider removing client if initial write fails badly?
                                 } else if (errno == EPIPE) {
                                     log_info("Client fd %d disconnected immediately (Broken Pipe on initial prompt)", client_fd);
                                     remove_client(client_fd, epoll_fd); // Clean up immediately
                                 }
                                 // If EAGAIN/EWOULDBLOCK, prompt might not be sent immediately.
                             }
                             // --- End initial prompt sending ---
                        }
                    }
                } else {
                    // Data available from existing client or client disconnected
                     if (current_events & EPOLLRDHUP) {
                        // Client closed connection or shutdown writing half (treat as disconnect)
                        log_info("Client fd %d disconnected (EPOLLRDHUP)", current_fd);
                        remove_client(current_fd, epoll_fd);
                    } else if (current_events & EPOLLIN) {
                        // Data is available to read
                        handle_client_read(current_fd, epoll_fd, cfg);
                    }
                    // Note: We are not explicitly handling EPOLLOUT here for simplicity.
                    // Writes are currently attempted directly within handle_client_read.
                }
            } // End loop through events
        } else {
            // epoll_wait timed out (n == 0)
            log_debug("epoll_wait timed out, checking for client timeouts.");
        }

        // --- Check for Client Timeouts (runs after processing events or timeout) ---
        time_t current_time = time(NULL);  // time(NULL) gets the current time
        char msg_buf[WRITE_BUFFER_SIZE]; // Buffer for timeout message
        int msg_len; 

        // Iterate backwards to allow safe removal while iterating
        for (int i = client_count - 1; i >= 0; --i) {
            if (clients[i]) { // Check if client exists at this index
                // Use cfg->timeout_val for the check
                if (cfg->timeout_val > 0) { // Only check if timeout is enabled (> 0)
                    double elapsed = difftime(current_time, clients[i]->last_activity);  // Calculate elapsed time since last activity
                    if (elapsed >= cfg->timeout_val) {
                        log_info("Client fd %d timed out after %.0f seconds of inactivity (limit: %d). Disconnecting.", clients[i]->fd, elapsed, cfg->timeout_val);
                        // Inform client using write_all
                        msg_len = snprintf(msg_buf, sizeof(msg_buf), "Connection timed out due to inactivity.\n");
                        if (msg_len > 0) {
                            write_all(clients[i]->fd, msg_buf, msg_len);
                            // Ignore errors on write_all, we are disconnecting
                        }
                        remove_client(clients[i]->fd, epoll_fd); // remove_client handles epoll removal and closing fd
                    }
                }
            }
        }
        // --- End Timeout Check ---

    } // End main event loop

cleanup: // Label for cleanup jump
    // --- Cleanup ---
    log_info("Shutting down server.");
    // Iterate through the current count of clients
    log_debug("Cleaning up %d clients.", client_count);

    for (int i = client_count - 1; i >= 0; --i) {
        if (clients[i]) {
             log_debug("Cleaning up client fd %d at index %d", clients[i]->fd, i);
             // Call simplified remove/cleanup for remaining clients
             if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, clients[i]->fd, NULL) == -1) {
                 log_perror("epoll_ctl(DEL) failed during shutdown");
             }
             close(clients[i]->fd);
             free(clients[i]->buffer);
             free(clients[i]);
             clients[i] = NULL; // Mark as cleaned
        }
    }
    free(clients); // Free the dynamic array itself
    clients = NULL;
    client_count = 0;
    client_capacity = 0;

    close(listen_fd);
    close(epoll_fd);
    log_info("Server shutdown complete.");
}