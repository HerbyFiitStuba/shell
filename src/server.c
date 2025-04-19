#include "server.h"
#include "logger.h"
#include "prompt.h" // <-- Add include for prompt generation
#include "executor.h" // <-- Include for execute_command_sequence
#include "parser.h" // <-- Include for tokenize, parse, free_tokens
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
#include <fcntl.h>      // Added for fcntl (non-blocking)
#include <signal.h>     // Added for sig_atomic_t
#include <stdio.h>      // For dprintf (writing help/errors to fd)

// Initial size and growth step for the dynamic buffer
static const size_t INITIAL_BUFFER_SIZE = 128; // Initial size of the buffer
static const size_t GROWTH_INCREMENT = 128; // Growth step for the buffer
static const size_t MAX_EVENTS = 64;       // Max events per epoll_wait call

// --- Global flag to signal server halt ---
static volatile sig_atomic_t halt_requested = 0;

// Structure to hold state for each client
typedef struct {
    int fd;
    char *buffer;
    size_t buffer_size;     // Current number of bytes stored
    size_t buffer_capacity; // Current allocated capacity
} client_state;

// --- Global Client Management (Dynamic Array) ---
static client_state **clients = NULL; // Pointer to array of client_state pointers
static int client_count = 0;          // Number of active clients
static int client_capacity = 0;       // Allocated capacity of the clients array
#define CLIENT_ARRAY_GROWTH 16        // How many slots to add when resizing

// --- Helper Functions ---

// Make a socket non-blocking
static int make_socket_non_blocking(int sfd) {
    int flags = fcntl(sfd, F_GETFL, 0);
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

        // Optional: Shrink array if count is much smaller than capacity (e.g., count < capacity / 4)
        // This adds complexity and potential realloc overhead, so omitted for now.

    } else {
        log_error("Attempted to remove non-existent client fd %d", fd);
        // Might still need to close fd if index wasn't found but fd is valid
        // Check if fd is valid before closing? (More complex)
        close(fd); // Close fd just in case, though state wasn't found
    }
}

// Handle data received from a client (No changes needed in this function itself)
static void handle_client_read(int fd, int epoll_fd) {
    int index = find_client_index(fd);
    if (index == -1) {
        log_error("Read event for unknown client fd %d", fd);
        remove_client(fd, epoll_fd); // Clean up this unknown fd
        return;
    }

    client_state *client = clients[index];
    char read_buf[512]; // Temporary buffer for reading chunks

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

    // Process complete lines from the buffer
    char *line_start = client->buffer;
    char *newline_pos;
    while ((newline_pos = memchr(line_start, '\n', client->buffer_size - (line_start - client->buffer))) != NULL) {
        // size_t line_len = newline_pos - line_start;

        // Temporarily null-terminate the line for processing
        *newline_pos = '\0';
        
        // --- Replace echo with parsing and execution ---
        log_debug("Received line from fd %d: %s", fd, line_start);
        
        // 1. Tokenize
        Token *tokens = tokenize(line_start);
        if (tokens == NULL) {
            log_error("Tokenizer failed for input from fd %d", fd);
            // Optionally send an error message back to the client
            dprintf(fd, "Error: Tokenization failed.\n");
            // Continue to next line or handle error further
        } else {
            // 2. Parse
            Command *cmd_sequence = parse(tokens);
            if (cmd_sequence == NULL) {
                log_error("Parser failed for input from fd %d", fd);
                // Parser already prints syntax errors to stderr
                // Optionally send a generic error message back to the client
                dprintf(fd, "Error: Command parsing failed (syntax error?).\n");
            } else {
                // 3. Execute
                log_debug("Executing command sequence for fd %d", fd);
                int exec_result = execute_command_sequence(cmd_sequence, fd);
                log_debug("Execution finished for fd %d with result %d", fd, exec_result);
        
                // Handle execution results
                if (exec_result == EXEC_QUIT) {
                    log_info("Client fd %d requested quit.", fd);
                    remove_client(fd, epoll_fd); // Close connection
                    free_command_sequence(cmd_sequence); // Free parsed structure
                    free_tokens(tokens);                 // Free tokens
                    return; // Stop processing buffer for this client
                } else if (exec_result == EXEC_HALT) {
                    log_info("Halt command received from fd %d. Initiating server shutdown.", fd);
                    // Need a way to signal the main loop in server_run to break
                    // This might involve setting a global flag or using a pipe/signal
                    // For now, just log it. A proper implementation needs server loop modification.
                    // Example: Set a global flag `static volatile sig_atomic_t halt_requested = 0;`
                    //          and check it in the server_run loop.
                    halt_requested = 1; // Set the global flag to signal shutdown
                    dprintf(fd, "Server halt initiated.\n"); // Inform client
                    // The main loop will detect the flag and break
                } else if (exec_result == EXEC_ERROR) {        
                    log_error("Critical execution error for fd %d.", fd);
                    // Error message should have been sent to client via stderr redirection
                    // dprintf(fd, "Error: Command execution failed.\n"); // Optional generic message
                }
                // Free parsed structure if execution didn't cause immediate return
                free_command_sequence(cmd_sequence);
            }
            // Free tokens if parsing didn't cause immediate return
            free_tokens(tokens);
        }
        // --- End parsing and execution section ---

        // Restore newline if needed, though not strictly necessary as we advance past it
        // *newline_pos = '\n';

        // Advance line_start past the processed line (including the newline)
        line_start = newline_pos + 1;

        // --- Send prompt after processing command ---
        const char *prompt = generate_prompt();
        ssize_t prompt_len = strlen(prompt);
        if (write(fd, prompt, prompt_len) < 0) {
             if (errno != EPIPE && errno != EAGAIN && errno != EWOULDBLOCK) {
                log_perror("write prompt failed");
             } else if (errno == EPIPE) {
                 log_info("Client fd %d disconnected (Broken Pipe on prompt write)", fd);
                 remove_client(fd, epoll_fd);
                 return; // Client gone
             }
             // If EAGAIN/EWOULDBLOCK, prompt might not be sent immediately.
             // A full implementation would buffer this output.
        }
        // --- End prompt sending ---

    }

    // Remove processed data from the buffer by shifting remaining data
    size_t remaining_size = client->buffer_size - (line_start - client->buffer);
    if (remaining_size > 0 && line_start != client->buffer) {
        memmove(client->buffer, line_start, remaining_size);
    }
    client->buffer_size = remaining_size;

    // Optional: Shrink buffer if it's very large and mostly unused? (Can add complexity)
}


// --- Main Server Logic ---

int socket_setup(const Config *cfg) {
    int listen_fd;
    struct sockaddr_in serv_addr;
    int opt = 1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_perror("socket() failed");
        return -1;
    }

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

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(cfg->port);
    if (cfg->bind_addr[0] != '\0') {
        if (inet_pton(AF_INET, cfg->bind_addr, &serv_addr.sin_addr) <= 0) {
            log_error("inet_pton('%s') failed: %s", cfg->bind_addr, strerror(errno));
            close(listen_fd);
            return -1;
        }
    } else {
        serv_addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        log_perror("bind() failed");
        log_error("Failed to bind to %s:%d", cfg->bind_addr[0] ? cfg->bind_addr : "0.0.0.0", cfg->port);
        close(listen_fd);
        return -1;
    }

    // Increase backlog slightly? e.g., 128
    if (listen(listen_fd, 128) < 0) {
        log_perror("listen() failed");
        close(listen_fd);
        return -1;
    }

    log_info("Server listening non-blockingly on %s:%d",
             cfg->bind_addr[0] ? cfg->bind_addr : "0.0.0.0", cfg->port);
    return listen_fd;
}

void server_run(const Config *cfg) {
    // Ensure globals are initialized (should be automatic for static globals)
    clients = NULL;
    client_count = 0;
    client_capacity = 0;

    int listen_fd = socket_setup(cfg);
    if (listen_fd < 0) return;

    int epoll_fd = epoll_create1(0);
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

    // Main Event Loop
    // for (;;) { // Original loop
    while (!halt_requested) { // Loop until halt is requested
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); // Wait indefinitely

        // Check halt flag *immediately* after epoll_wait returns,
        // especially if wait could be interrupted by signals in the future.
        if (halt_requested) {
            log_info("Halt flag detected after epoll_wait, breaking loop.");
            break;
        }

        if (n == -1) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, safe to continue
            }
            log_perror("epoll_wait failed");
            break; // Exit loop on other errors
        }

        log_debug("epoll_wait returned %d events", n);

        for (int i = 0; i < n; i++) {
            uint32_t current_events = events[i].events;
            int current_fd = events[i].data.fd;

            if ((current_events & EPOLLERR) || (current_events & EPOLLHUP)) {
                // Error or hang up occurred on this fd.
                log_error("epoll error/hangup on fd %d", current_fd);
                // Check if it's the listening socket (unlikely, but possible)
                if (current_fd != listen_fd) {
                    remove_client(current_fd, epoll_fd); // Close and remove client
                } else {
                    log_error("Error on listening socket %d! Shutting down?", current_fd);
                    // Handle listening socket error (e.g., break loop)
                    goto cleanup; // Use goto for simplicity in this case
                }
            } else if (current_fd == listen_fd) {
                // New incoming connection(s)
                log_debug("Accepting new connections on listen_fd %d", listen_fd);
                while (1) {
                    struct sockaddr_in cli_addr;
                    socklen_t cli_len = sizeof(cli_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr*)&cli_addr, &cli_len);

                    if (client_fd == -1) {
                        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                            // We have processed all incoming connections.
                            log_debug("Finished accepting connections (EAGAIN/EWOULDBLOCK)");
                            break;
                        } else {
                            log_perror("accept failed");
                            break; // Error accepting
                        }
                    }

                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &cli_addr.sin_addr, client_ip, sizeof(client_ip));
                    log_info("Client connected from %s:%d (fd: %d)", client_ip, ntohs(cli_addr.sin_port), client_fd);

                    if (make_socket_non_blocking(client_fd) == -1) {
                        close(client_fd);
                        continue; // Failed to make non-blocking
                    }

                    int client_index = add_client(client_fd); // Use dynamic add_client
                    if (client_index == -1) {
                        log_error("Failed to add client state for fd %d (capacity %d)", client_fd, client_capacity);
                        close(client_fd); // Close socket if we can't manage state
                        continue;
                    }

                    event.data.fd = client_fd;
                    // Monitor for input, edge-triggered, and remote close/half-close
                    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1) {
                        log_perror("epoll_ctl(ADD client_fd) failed");
                        remove_client(client_fd, epoll_fd); // Clean up if add fails
                    } else {
                         log_debug("Added client fd %d to epoll", client_fd);
                         // --- Send initial prompt ---
                         const char *prompt = generate_prompt();
                         ssize_t prompt_len = strlen(prompt);
                         if (write(client_fd, prompt, prompt_len) < 0) {
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
                    handle_client_read(current_fd, epoll_fd);
                }
                // Note: We are not explicitly handling EPOLLOUT here for simplicity.
                // Writes are currently attempted directly within handle_client_read.
            }
        } // End loop through events
    } // End main event loop

cleanup: // Label for cleanup jump
    // --- Cleanup ---
    log_info("Shutting down server.");
    // Iterate through the current count of clients
    log_debug("Cleaning up %d clients.", client_count);
    // Iterate backwards to simplify removal logic (no shifting needed)
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