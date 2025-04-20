#include "executor.h"
#include "logger.h"
#include "parser.h"     // For Command structure
#include "tokenizer.h"  // For Token structure
#include "config.h"     // For Config structure
#include "server.h"     // For client_state structure
#include <stdio.h>      // For dprintf, snprintf
#include <stdlib.h>     // For exit, EXIT_FAILURE, EXIT_SUCCESS
#include <string.h>     // For strcmp, strerror
#include <unistd.h>     // For fork, execvp, pipe, dup2, close, STDIN_FILENO, etc.
#include <sys/wait.h>   // For waitpid
#include <sys/types.h>  // For pid_t, socket types
#include <fcntl.h>      // For open, O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC
#include <errno.h>      // For errno
#include <signal.h>     // For kill, SIGTERM
#include <sys/socket.h> // For getsockname, getpeername
#include <netinet/in.h> // For sockaddr_in
#include <arpa/inet.h>  // For inet_ntop
#include <limits.h>     // For INT_MAX with strtol

#define WRITE_BUFFER_SIZE 1024 // Buffer for formatting output before write

// --- Forward Declarations for Helper Functions ---
static int execute_pipeline(Command *pipeline_head, int client_fd, const Config *cfg, client_state **clients, int client_count);
static int handle_builtin(Command *cmd, int client_fd, const Config *cfg, client_state **clients, int client_count);
static void do_exec_child(Command *cmd, int client_fd, int pipe_in_fd, int pipe_out_fd, int (*all_pipe_fds)[2], int num_pipes, const Config *cfg, client_state **clients, int client_count); // Added server state params

// --- Helper Functions for Built-ins ---
static int print_help(int output_fd);
static int print_stat(int output_fd, const Config *cfg, client_state **clients, int client_count);
static int write_all(int fd, const char *buf, size_t len);


// --- Main Execution Function ---

/**
 * @brief Executes a parsed command sequence received from a client.
 */
// Updated signature
int execute_command_sequence(Command *cmd_sequence, int client_fd, const Config *cfg, client_state **clients, int client_count) {
    Command *current_pipeline = cmd_sequence;
    int result = EXEC_OK;

    while (current_pipeline != NULL) {
        log_debug("Executing pipeline starting with command: %s",
                  (current_pipeline->argv && current_pipeline->argv[0]) ? current_pipeline->argv[0] : "(empty)");

        // Pass server state down
        result = execute_pipeline(current_pipeline, client_fd, cfg, clients, client_count);

        // Check result from pipeline execution
        if (result == EXEC_HALT || result == EXEC_QUIT || result == EXEC_ERROR) {
            log_debug("Pipeline execution returned %d, stopping sequence.", result);
            return result; // Propagate halt, quit, or error immediately
        }

        // Move to the next command sequence (after ';')
        current_pipeline = current_pipeline->next_command_sequence;
    }

    log_debug("Command sequence finished with result: %d", result);
    return result; // Should be EXEC_OK if loop finished
}

// --- Helper Functions ---

// Helper function to write all data, handling partial writes and EINTR
static int write_all(int fd, const char *buf, size_t len) {
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t written = write(fd, buf + total_written, len - total_written);
        if (written < 0) {
            if (errno == EINTR) continue; // Interrupted system call, try again
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "ERROR: write failed in write_all (fd=%d): %s\n", fd, strerror(errno));
            write(STDERR_FILENO, err_buf, strlen(err_buf)); // write error message to stderr
            return -1; // Indicate failure
        }
        if (written == 0 && len > 0) {
            // This shouldn't happen but we handle it for safety measures
            char err_buf[128];
            snprintf(err_buf, sizeof(err_buf), "ERROR: write returned 0 unexpectedly (fd=%d)\n", fd);
            write(STDERR_FILENO, err_buf, strlen(err_buf)); // write error message to stderr
            return -1; // Indicate failure
        }
        total_written += (size_t)written;
    }
    return 0; // Indicate success
}


/**
 * @brief Executes a single pipeline (one or more commands linked by |).
 * Handles built-ins appropriately (terminating, propagable, or none).
 */
// Updated signature
static int execute_pipeline(Command *pipeline_head, int client_fd, const Config *cfg, client_state **clients, int client_count) {
    char err_msg[WRITE_BUFFER_SIZE]; // Buffer for error messages
    int err_len; 

    // --- Check the *first* command for terminating or special built-ins ---
    int first_cmd_type = handle_builtin(pipeline_head, client_fd, cfg, clients, client_count);

    // Handle terminating built-ins (quit, halt)
    if (first_cmd_type == EXEC_QUIT || first_cmd_type == EXEC_HALT) {
        if (pipeline_head->next_command_in_pipeline == NULL) {
            // Single command pipeline, execute terminating built-in directly
            log_debug("Executing terminating built-in directly: %s", pipeline_head->argv[0]);
            return first_cmd_type;
        } else {
            // Terminating built-in cannot be in a pipeline
            log_error("Terminating built-in '%s' cannot be used in a pipeline.", pipeline_head->argv[0]);
            err_len = snprintf(err_msg, sizeof(err_msg), "Error: Built-in command '%s' cannot be used in a pipeline.\n", pipeline_head->argv[0]);
            if (err_len > 0) write_all(client_fd, err_msg, err_len); // Inform client
            return EXEC_ERROR;
        }
    }

    // Handle 'abort n' built-in
    if (first_cmd_type == EXEC_ABORT_N) {
        if (pipeline_head->next_command_in_pipeline == NULL) {
            // 'abort' must be a single command
            log_debug("Processing 'abort n' command from fd %d", client_fd);
            // Validate arguments: must have exactly one argument 
            if (pipeline_head->argv[1] == NULL) {
                log_error("Invalid 'abort' usage: requires exactly one argument (client index).");
                err_len = snprintf(err_msg, sizeof(err_msg), "Error: Usage: abort <client_index>\n");
                if (err_len > 0) write_all(client_fd, err_msg, err_len);
                return EXEC_ERROR;
            }

            // Validate the argument is a non-negative integer within bounds
            char *endptr;
            long val;
            errno = 0;
            val = strtol(pipeline_head->argv[1], &endptr, 10);

            if (errno != 0 || endptr == pipeline_head->argv[1] || *endptr != '\0' || val < 0 || val >= client_count || val > INT_MAX) {
                 log_error("Invalid 'abort' index '%s': must be a valid integer between 0 and %d.", pipeline_head->argv[1], client_count - 1);
                 err_len = snprintf(err_msg, sizeof(err_msg), "Error: Invalid client index '%s'. Must be between 0 and %d.\n", pipeline_head->argv[1], client_count - 1);
                 if (err_len > 0) write_all(client_fd, err_msg, err_len);
                 return EXEC_ERROR;
            }

            // Validation passed, signal server to perform the abort
            log_debug("'abort %ld' validated, returning EXEC_ABORT_N.", val);
            return EXEC_ABORT_N; // Server will handle the actual abort

        } else {
            // 'abort' cannot be in a pipeline
            log_error("'abort' command cannot be used in a pipeline.");
            err_len = snprintf(err_msg, sizeof(err_msg), "Error: Built-in command 'abort' cannot be used in a pipeline.\n");
            if (err_len > 0) write_all(client_fd, err_msg, err_len); // Inform client
            return EXEC_ERROR;
        }
    }


    // --- Proceed with pipeline execution for propagable built-ins or external commands ---
    int num_cmds = 0;
    Command *cmd_iter = pipeline_head;
    while (cmd_iter != NULL) {
        // Basic check: ensure command exists
        if (!cmd_iter->argv || !cmd_iter->argv[0]) {
             log_error("Invalid command structure: empty command in pipeline.");
             // Use write_all for error message
             err_len = snprintf(err_msg, sizeof(err_msg), "Error: Invalid empty command in pipeline.\n");
             if (err_len > 0) write_all(client_fd, err_msg, err_len);
             return EXEC_ERROR; // Indicate an error in the structure
        }
        num_cmds++;
        cmd_iter = cmd_iter->next_command_in_pipeline;
    }

    if (num_cmds == 0) { // Should have been caught above, but double-check
        return EXEC_OK;
    }

    log_debug("Pipeline has %d stages.", num_cmds);

    pid_t *pids = malloc(num_cmds * sizeof(pid_t));
    int num_pipes = num_cmds - 1;
    // Allocate space for pipe FDs only if needed
    int (*pipe_fds)[2] = (num_pipes > 0) ? malloc(num_pipes * sizeof(int[2])) : NULL;

    if (!pids || (num_pipes > 0 && !pipe_fds)) {
        log_perror("Failed to allocate memory for pids or pipes");
        free(pids);
        free(pipe_fds);
        return EXEC_ERROR;
    }

    // --- Create all pipes first ---
    for (int i = 0; i < num_pipes; i++) {
        if (pipe(pipe_fds[i]) == -1) {
            log_perror("pipe failed");
            // Close any pipes already created
            for (int k = 0; k < i; k++) {
                close(pipe_fds[k][0]);  // Close read end
                close(pipe_fds[k][1]);  // Close write end
            }
            free(pids);
            free(pipe_fds);
            return EXEC_ERROR;
        }
        log_debug("Created pipe %d: read_fd=%d, write_fd=%d", i, pipe_fds[i][0], pipe_fds[i][1]);
    }

    // --- Fork all children ---
    cmd_iter = pipeline_head;
    for (int i = 0; i < num_cmds; i++) {
        pids[i] = fork();

        if (pids[i] < 0) {
            log_perror("fork failed");
            // Cleanup: kill already created children, close all pipes, wait, free, return error
            for(int k=0; k<i; k++) kill(pids[k], SIGTERM);
            for(int p=0; p<num_pipes; p++) { close(pipe_fds[p][0]); close(pipe_fds[p][1]); }
            for(int k=0; k<i; k++) waitpid(pids[k], NULL, 0);
            free(pids);
            free(pipe_fds);
            return EXEC_ERROR;

        } else if (pids[i] == 0) {
            // --- Child Process ---
            int pipe_in = (i == 0) ? -1 : pipe_fds[i-1][0]; // Read from previous pipe's read end
            int pipe_out = (i == num_cmds - 1) ? -1 : pipe_fds[i][1]; // Write to current pipe's write end

            // Pass all pipe fds for cleanup after dup2, and server state for built-ins
            do_exec_child(cmd_iter, client_fd, pipe_in, pipe_out, pipe_fds, num_pipes, cfg, clients, client_count);
            exit(EXIT_FAILURE); // Should not be reached

        } else {
            // --- Parent Process ---
            log_debug("Forked child %d (pid %d) for command '%s'", i, pids[i], cmd_iter->argv[0]);
            // Parent does NOT close pipe ends here
        }
        cmd_iter = cmd_iter->next_command_in_pipeline;
    } // End for loop creating children

    // --- Parent: Close ALL pipe ends NOW ---
    // After all children are forked, the parent doesn't need any pipe ends open.
    log_debug("Parent closing all %d pipe ends.", num_pipes * 2);
    for (int i = 0; i < num_pipes; i++) {
        // Check if close fails, although not much we can do if it does
        if (close(pipe_fds[i][0]) == -1) log_perror("close pipe read end failed in parent");
        if (close(pipe_fds[i][1]) == -1) log_perror("close pipe write end failed in parent");
    }

    // --- Parent: Wait for all children in the pipeline ---
    int status;
    int final_result = EXEC_OK;
    log_debug("Parent waiting for %d children.", num_cmds);
    for (int i = 0; i < num_cmds; i++) {
        if (waitpid(pids[i], &status, 0) == -1) {
            log_perror("waitpid failed");
            final_result = EXEC_ERROR; // Mark error, but continue waiting for others if possible
        } else {
            if (WIFEXITED(status)) {
                log_debug("Child pid %d exited with status %d", pids[i], WEXITSTATUS(status));
                if (WEXITSTATUS(status) != 0) {
                    log_error("Child pid %d exited with error status %d", pids[i], WEXITSTATUS(status));
                    // Check if this was a built-in that failed
                    if (first_cmd_type == EXEC_IS_PROPAGABLE) {
                        final_result = EXEC_ERROR; // Mark error for propagable built-ins
                    } else {
                        final_result = WEXITSTATUS(status); // Propagate the exit status
                    }
                } else {
                    log_debug("Child pid %d exited successfully.", pids[i]);
                }
            } else if (WIFSIGNALED(status)) {
                log_error("Child pid %d terminated by signal %d", pids[i], WTERMSIG(status));
                // Treat signal termination (like SIGPIPE, SIGSEGV) as an error
                final_result = EXEC_ERROR;
            }
        }
    }

    free(pids);
    free(pipe_fds);
    log_debug("Pipeline finished, returning %d", final_result);
    return final_result;
}

/**
 * @brief Checks if a command is a built-in and returns its type.
 * Does NOT execute the built-in here, only identifies it.
 * @param cmd The command structure.
 * @param client_fd The client's file descriptor.
 * @param cfg Server configuration.
 * @param clients Array of client states.
 * @param client_count Number of clients.
 * @return EXEC_QUIT, EXEC_HALT, EXEC_ABORT_N, EXEC_IS_PROPAGable, or EXEC_NOT_BUILTIN.
 */
static int handle_builtin(Command *cmd, int client_fd, const Config *cfg, client_state **clients, int client_count) {
    // Parameters client_fd, cfg, clients, client_count are unused now,
    (void)client_fd;
    (void)cfg;
    (void)clients;
    (void)client_count;

    if (!cmd || !cmd->argv || !cmd->argv[0]) {
        return EXEC_NOT_BUILTIN; // Treat empty command as not a built-in
    }

    const char *command_name = cmd->argv[0];
    log_debug("Checking built-in type for: %s", command_name);

    if (strcmp(command_name, "quit") == 0) {
        return EXEC_QUIT;
    } else if (strcmp(command_name, "halt") == 0) {
        return EXEC_HALT;
    } else if (strcmp(command_name, "abort") == 0) {
        return EXEC_ABORT_N; // Identify abort, validation happens in execute_pipeline
    } else if (strcmp(command_name, "help") == 0) {
        return EXEC_IS_PROPAGABLE;
    } else if (strcmp(command_name, "stat") == 0) {
        return EXEC_IS_PROPAGABLE;
    }

    return EXEC_NOT_BUILTIN; // Not a built-in command we handle
}

/**
 * @brief Sets up I/O redirection and executes the command (external or built-in) in the child process.
 * This function is intended to be called *only* after fork().
 * It does not return on success (process image is replaced by execvp or built-in finishes).
 * It calls exit() on failure or after successful built-in execution.
 *
 * @param cmd The command to execute.
 * @param client_fd The client's socket fd.
 * @param pipe_in_fd Read end of the input pipe.
 * @param pipe_out_fd Write end of the output pipe.
 * @param all_pipe_fds Array containing all pipe FDs created by the parent.
 * @param num_pipes Total number of pipes created.
 * @param cfg Server configuration.
 * @param clients Array of client states.
 * @param client_count Number of clients.
 */
static void do_exec_child(Command *cmd, int client_fd, int pipe_in_fd, int pipe_out_fd, int (*all_pipe_fds)[2], int num_pipes, const Config *cfg, client_state **clients, int client_count) {
    log_debug("Child (pid %d) setting up for command '%s'", getpid(), cmd->argv[0]);
    char err_msg[WRITE_BUFFER_SIZE]; // Buffer for error messages
    int err_len; 

    // --- Input Redirection ---
    if (cmd->input_file) {
        log_debug("Redirecting STDIN from file: %s", cmd->input_file);
        int fd_in = open(cmd->input_file, O_RDONLY);
        if (fd_in == -1) {
            log_perror("open input file failed");
            // Use write_all for error message
            err_len = snprintf(err_msg, sizeof(err_msg), "Error opening input file '%s': %s\n", cmd->input_file, strerror(errno));
            if (err_len > 0) write_all(client_fd, err_msg, err_len); // Write to original client fd before redirection
            exit(EXIT_FAILURE);
        }
        if (dup2(fd_in, STDIN_FILENO) == -1) {
            log_perror("dup2 STDIN failed");
            close(fd_in); // Close original fd even on error
            exit(EXIT_FAILURE);
        }
        close(fd_in); // Close original fd after successful dup2
    } else if (pipe_in_fd != -1) {
        log_debug("Redirecting STDIN from pipe_in_fd: %d", pipe_in_fd);
        if (dup2(pipe_in_fd, STDIN_FILENO) == -1) {
            log_perror("dup2 STDIN from pipe failed");
            exit(EXIT_FAILURE);
        }
        // No need to close pipe_in_fd here, will be closed in the loop below
    }
    // Else: STDIN remains default

    // Flush buffers before redirecting output/error streams
    fflush(stdout);
    fflush(stderr);

    // --- Output Redirection ---
    if (cmd->output_file) {
        log_debug("Redirecting STDOUT/STDERR to file: %s", cmd->output_file);
        int fd_out = open(cmd->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd_out == -1) {
            log_perror("open output file failed");
            // Use write_all for error message
            err_len = snprintf(err_msg, sizeof(err_msg), "Error opening output file '%s': %s\n", cmd->output_file, strerror(errno));
             // Write to STDERR_FILENO which might be client_fd or a pipe write end
            if (err_len > 0) write_all(STDERR_FILENO, err_msg, err_len);
            exit(EXIT_FAILURE);
        }
        if (dup2(fd_out, STDOUT_FILENO) == -1) {
            log_perror("dup2 STDOUT failed");
            close(fd_out);
            exit(EXIT_FAILURE);
        }
        // Also redirect STDERR to the output file
        if (dup2(fd_out, STDERR_FILENO) == -1) {
             log_perror("dup2 STDERR failed");
             close(fd_out);
             exit(EXIT_FAILURE);
        }
        close(fd_out); // Close original fd after successful dup2
    } else if (pipe_out_fd != -1) {
        log_debug("Redirecting STDOUT to pipe_out_fd: %d", pipe_out_fd);
        if (dup2(pipe_out_fd, STDOUT_FILENO) == -1) {
            log_perror("dup2 STDOUT to pipe failed");
            exit(EXIT_FAILURE);
        }
        // Redirect STDERR to client
        if (dup2(client_fd, STDERR_FILENO) == -1) {
             log_perror("dup2 STDERR to client_fd failed");
             exit(EXIT_FAILURE);
        }
    } else {
        // No output file, no output pipe -> redirect STDOUT and STDERR to client
        log_debug("Redirecting STDOUT/STDERR to client_fd: %d", client_fd);
        if (dup2(client_fd, STDOUT_FILENO) == -1) {
            log_perror("dup2 STDOUT to client_fd failed");
            exit(EXIT_FAILURE);
        }
        if (dup2(client_fd, STDERR_FILENO) == -1) {
            log_perror("dup2 STDERR to client_fd failed");
            exit(EXIT_FAILURE);
        }
        // Don't need to close client_fd here, execvp handles it.
    }

    // --- Close ALL original pipe FDs passed from parent ---
    // After dup2, the child only needs STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO.
    // It must close all ends of all pipes it inherited, even the ones it used for dup2.
    log_debug("Child closing its copies of %d pipe ends.", num_pipes * 2);
    for (int p = 0; p < num_pipes; ++p) {
        if (all_pipe_fds[p][0] != -1) close(all_pipe_fds[p][0]);
        if (all_pipe_fds[p][1] != -1) close(all_pipe_fds[p][1]);
    }

    // --- Determine Command Type and Execute ---
    int builtin_type = handle_builtin(cmd, client_fd, cfg, clients, client_count);

    if (builtin_type == EXEC_IS_PROPAGABLE) {
        log_info("Child (pid %d) executing propagable built-in: %s", getpid(), cmd->argv[0]);
        int result = -1;
        if (strcmp(cmd->argv[0], "help") == 0) {
            result = print_help(STDOUT_FILENO); // Output goes to redirected STDOUT
        } else if (strcmp(cmd->argv[0], "stat") == 0) {
            result = print_stat(STDOUT_FILENO, cfg, clients, client_count); // Output goes to redirected STDOUT
        } else {
            // Should not happen if handle_builtin is correct
            log_error("Internal error: Unknown propagable built-in '%s'", cmd->argv[0]);
            exit(EXIT_FAILURE);
        }

        if (result == 0) {
            exit(EXIT_SUCCESS); // Built-in executed successfully
        } else {
            log_error("Built-in command '%s' failed during execution.", cmd->argv[0]);
            exit(EXIT_FAILURE); // Built-in failed
        }

    } else if (builtin_type == EXEC_NOT_BUILTIN) {
        log_info("Child (pid %d) executing external command: %s", getpid(), cmd->argv[0]);
        execvp(cmd->argv[0], cmd->argv);

        // --- execvp Failed ---
        log_perror("execvp failed");
        // Use write_all for error message
        err_len = snprintf(err_msg, sizeof(err_msg), "Error executing command '%s': %s\n", cmd->argv[0], strerror(errno));
        if (err_len > 0) write_all(STDERR_FILENO, err_msg, err_len); // Write to redirected STDERR
        exit(EXIT_FAILURE); // Terminate child on execvp failure

    } else {
        // EXEC_QUIT, EXEC_HALT, EXEC_ABORT_N - should not happen in child
        log_error("Internal error: Non-propagable built-in '%s' reached child process.", cmd->argv[0]);
        exit(EXIT_FAILURE);
    }
}


/**
 * @brief Prints server status information (listening sockets, connected clients)
 *        to the specified file descriptor using write().
 * @return 0 on success, -1 on failure.
 */
// Updated signature and implementation
static int print_stat(int output_fd, const Config *cfg, client_state **clients, int client_count) {
    char buffer[WRITE_BUFFER_SIZE];
    int bytes_written; // Renamed from n
    int write_failed = 0; // Flag to track write errors
    time_t current_time = time(NULL); // Get current time once

    bytes_written = snprintf(buffer, sizeof(buffer), "--- Server Status ---\n");
    if (bytes_written > 0 && write_all(output_fd, buffer, bytes_written) < 0) write_failed = 1;

    // --- Listening Sockets ---
    if (!write_failed) {
        bytes_written = snprintf(buffer, sizeof(buffer), "Listening on: %s:%d\n",
                cfg->bind_addr[0] ? cfg->bind_addr : "0.0.0.0 (All Interfaces)",
                cfg->port);
        if (bytes_written > 0 && write_all(output_fd, buffer, bytes_written) < 0) write_failed = 1;
    }

    // --- Timeout Configuration ---
    if (!write_failed) {
        if (cfg->timeout_val > 0) {
            bytes_written = snprintf(buffer, sizeof(buffer), "Client Inactivity Timeout: %d seconds\n", cfg->timeout_val);
        } else {
            bytes_written = snprintf(buffer, sizeof(buffer), "Client Inactivity Timeout: Disabled\n");
        }
        if (bytes_written > 0 && write_all(output_fd, buffer, bytes_written) < 0) write_failed = 1;
    }

    // --- Connected Clients ---
    if (!write_failed) {
        bytes_written = snprintf(buffer, sizeof(buffer), "Connected Clients (%d):\n", client_count);
        if (bytes_written > 0 && write_all(output_fd, buffer, bytes_written) < 0) write_failed = 1;
    }

    if (!write_failed) {
        if (client_count == 0) {
            bytes_written = snprintf(buffer, sizeof(buffer), "  (None)\n");
            if (bytes_written > 0 && write_all(output_fd, buffer, bytes_written) < 0) write_failed = 1;
        } else {
            for (int i = 0; i < client_count && !write_failed; ++i) {
                if (clients[i]) { // Check if the slot is valid
                    int fd = clients[i]->fd;
                    struct sockaddr_in addr;
                    socklen_t addr_len = sizeof(addr);
                    char ip_str[INET_ADDRSTRLEN];
                    int port = 0;
                    double elapsed = difftime(current_time, clients[i]->last_activity);
                    double remaining = (cfg->timeout_val > 0) ? (cfg->timeout_val - elapsed) : -1.0; // -1 indicates disabled/irrelevant

                    if (getpeername(fd, (struct sockaddr*)&addr, &addr_len) == 0) {
                        inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str));
                        port = ntohs(addr.sin_port);
                        if (remaining >= 0) {
                            bytes_written = snprintf(buffer, sizeof(buffer), "  - Client %d: fd=%d, Addr=%s:%d, Buf=%zu/%zu, Timeout=%.0fs\n",
                                    i, fd, ip_str, port, clients[i]->buffer_size, clients[i]->buffer_capacity, remaining > 0 ? remaining : 0); // Show 0 if already timed out
                        } else { // Timeout disabled
                             bytes_written = snprintf(buffer, sizeof(buffer), "  - Client %d: fd=%d, Addr=%s:%d, Buf=%zu/%zu, Timeout=N/A\n",
                                    i, fd, ip_str, port, clients[i]->buffer_size, clients[i]->buffer_capacity);
                        }
                    } else {
                        // Log error getting peer name, but still print basic info
                        log_perror("getpeername failed for fd"); // Note: log_perror might not be ideal in child
                         if (remaining >= 0) {
                            bytes_written = snprintf(buffer, sizeof(buffer), "  - Client %d: fd=%d, Addr=(Error), Buf=%zu/%zu, Timeout=%.0fs\n",
                                    i, fd, clients[i]->buffer_size, clients[i]->buffer_capacity, remaining > 0 ? remaining : 0);
                         } else {
                             bytes_written = snprintf(buffer, sizeof(buffer), "  - Client %d: fd=%d, Addr=(Error), Buf=%zu/%zu, Timeout=N/A\n",
                                    i, fd, clients[i]->buffer_size, clients[i]->buffer_capacity);
                         }
                    }
                     if (bytes_written > 0 && write_all(output_fd, buffer, bytes_written) < 0) write_failed = 1;

                } else {
                     bytes_written = snprintf(buffer, sizeof(buffer), "  - Client %d: (Invalid state pointer)\n", i);
                     if (bytes_written > 0 && write_all(output_fd, buffer, bytes_written) < 0) write_failed = 1;
                }
            }
        }
    }

    if (!write_failed) {
        bytes_written = snprintf(buffer, sizeof(buffer), "---------------------\n");
        if (bytes_written > 0 && write_all(output_fd, buffer, bytes_written) < 0) write_failed = 1;
    }

    return write_failed ? -1 : 0; // Return -1 if any write failed
}


/**
 * @brief Prints help information to the specified file descriptor using write().
 * @return 0 on success, -1 on failure.
 */
static int print_help(int output_fd) {
    int write_failed = 0;

    const char *help_lines[] = {
        "Author: [Matej Herzog]\n", 
        "Usage: shell [-s|-c] [-p port] [-i bind_addr] [-v] [-l logfile] [-t timeout] [-d] [-h]\n",
        "  -s : Run in server mode (default)\n",
        "  -c : Run in client mode\n",
        "  -p port : Port number (required for server, optional for client)\n",
        "  -i addr : IP address to bind server to (default: all)\n",
        "  -v : Enable verbose output to stderr\n",
        "  -l file : Log output to specified file (default: stderr)\n",
        "  -t secs : Client inactivity timeout in seconds (default: 600)\n",
        "  -d : Run server as a daemon process\n", 
        "  -h : Display this help message\n\n",
        "Built-in Commands:\n",
        "  help : Display this help message.\n",
        "  quit : Disconnect from the server.\n",
        "  halt : Terminate the server process.\n",
        "  stat : Show server status (listening sockets, connected clients).\n\n",
        "  abort n : Terminate the client with index n (0-based).\n",
        "Special Characters:\n",
        "  # : Start a comment (ignored).\n",
        "  ; : Separate commands to be executed sequentially.\n",
        "  | : Pipe the output of one command to the input of the next.\n",
        "  < file : Redirect standard input from a file.\n",
        "  > file : Redirect standard output to a file (overwrite).\n",
        NULL // End of help lines
    };

    for (int i = 0; help_lines[i] != NULL && !write_failed; ++i) {
        // Use write_all to handle partial writes and errors
        if (write_all(output_fd, help_lines[i], strlen(help_lines[i])) < 0) {
            write_failed = 1;
        }
    }

    return write_failed ? -1 : 0;
}