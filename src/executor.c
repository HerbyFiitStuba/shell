#include "executor.h"
#include "logger.h"
#include <stdio.h>      // For dprintf (writing help/errors to fd)
#include <stdlib.h>     // For exit, EXIT_FAILURE, EXIT_SUCCESS
#include <string.h>     // For strcmp
#include <unistd.h>     // For fork, execvp, pipe, dup2, close, STDIN_FILENO, etc.
#include <sys/wait.h>   // For waitpid
#include <sys/types.h>  // For pid_t
#include <fcntl.h>      // For open, O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC
#include <errno.h>      // For errno
#include <signal.h>     // For kill, SIGTERM

// --- Forward Declarations for Helper Functions ---
static int execute_pipeline(Command *pipeline_head, int client_fd);
static int handle_builtin(Command *cmd, int client_fd);
// Modified signature for do_exec_child
static void do_exec_child(Command *cmd, int client_fd, int pipe_in_fd, int pipe_out_fd, int (*all_pipe_fds)[2], int num_pipes);
static void print_help(int output_fd);

// --- Main Execution Function ---

/**
 * @brief Executes a parsed command sequence received from a client.
 */
int execute_command_sequence(Command *cmd_sequence, int client_fd) {
    Command *current_pipeline = cmd_sequence;
    int result = EXEC_OK;

    while (current_pipeline != NULL) {
        log_debug("Executing pipeline starting with command: %s",
                  (current_pipeline->argv && current_pipeline->argv[0]) ? current_pipeline->argv[0] : "(empty)");

        result = execute_pipeline(current_pipeline, client_fd);

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

/**
 * @brief Executes a single pipeline (one or more commands linked by |).
 * Handles built-ins for single-command pipelines.
 */
static int execute_pipeline(Command *pipeline_head, int client_fd) {
    // --- Check for Built-in Commands (only if it's a single command pipeline) ---
    if (pipeline_head->next_command_in_pipeline == NULL) {
        int builtin_result = handle_builtin(pipeline_head, client_fd);
        if (builtin_result != EXEC_OK) {
            // Built-in handled (or wants halt/quit), return its status
            return builtin_result;
        }
        // If builtin_result is EXEC_OK, it means it wasn't a terminating built-in,
        // so proceed to execute it as an external command below.
    }

    // --- External Command Execution (Single or Pipeline) ---
    int num_cmds = 0;
    Command *cmd_iter = pipeline_head;
    while (cmd_iter != NULL) {
        // Basic check: ensure command exists
        if (!cmd_iter->argv || !cmd_iter->argv[0]) {
             log_error("Invalid command structure: empty command in pipeline.");
             dprintf(client_fd, "Error: Invalid empty command in pipeline.\n");
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

            // Pass all pipe fds for cleanup after dup2
            do_exec_child(cmd_iter, client_fd, pipe_in, pipe_out, pipe_fds, num_pipes);
            exit(EXIT_FAILURE); // Should not be reached

        } else {
            // --- Parent Process (minimal logic in loop) ---
            log_debug("Forked child %d (pid %d) for command '%s'", i, pids[i], cmd_iter->argv[0]);
            // Parent does NOT close pipe ends here anymore
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
                    // Optionally report non-zero exit status as an error or info
                    // final_result = EXEC_ERROR; // Decide if any non-zero exit is an error
                    // Let's not treat non-zero exit as EXEC_ERROR for now,
                    // unless it was due to a signal.
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
 * @brief Checks and handles built-in commands.
 * Only considers the command if it's the only one in its pipeline.
 * @param cmd The command structure.
 * @param client_fd The client's file descriptor.
 * @return EXEC_QUIT if 'quit', EXEC_HALT if 'halt', EXEC_OK otherwise (including if 'help' or not a built-in).
 */
static int handle_builtin(Command *cmd, int client_fd) {
    // Built-ins only make sense as the sole command in a "pipeline"
    if (cmd->next_command_in_pipeline != NULL) {
        return EXEC_OK; // Not a single command, treat as external
    }
    if (!cmd->argv || !cmd->argv[0]) {
        return EXEC_OK; // Empty command
    }

    const char *command_name = cmd->argv[0];
    log_debug("Checking for built-in: %s", command_name);

    if (strcmp(command_name, "quit") == 0) {
        log_info("Executing built-in 'quit' for fd %d", client_fd);
        // The actual closing of fd happens in server.c based on this return code
        return EXEC_QUIT;
    } else if (strcmp(command_name, "halt") == 0) {
        log_info("Executing built-in 'halt'");
        // The server loop termination happens in server.c based on this return code
        return EXEC_HALT;
    } else if (strcmp(command_name, "help") == 0) {
        log_info("Executing built-in 'help' for fd %d", client_fd);
        print_help(client_fd);
        return EXEC_OK; // Help executed successfully, continue normally
    }

    return EXEC_OK; // Not a built-in command we handle here
}

/**
 * @brief Sets up I/O redirection and executes the command in the child process.
 * This function is intended to be called *only* after fork().
 * It does not return on success (process image is replaced by execvp).
 * It calls exit() on failure.
 *
 * @param cmd The command to execute.
 * @param client_fd The client's socket fd (used for default output/error).
 * @param pipe_in_fd Read end of the input pipe for this child, or -1 if none.
 * @param pipe_out_fd Write end of the output pipe for this child, or -1 if none.
 * @param all_pipe_fds Array containing all pipe FDs created by the parent.
 * @param num_pipes Total number of pipes created.
 */
static void do_exec_child(Command *cmd, int client_fd, int pipe_in_fd, int pipe_out_fd, int (*all_pipe_fds)[2], int num_pipes) {
    log_debug("Child (pid %d) setting up for command '%s'", getpid(), cmd->argv[0]);

    // --- Input Redirection ---
    if (cmd->input_file) {
        log_debug("Redirecting STDIN from file: %s", cmd->input_file);
        int fd_in = open(cmd->input_file, O_RDONLY);
        if (fd_in == -1) {
            log_perror("open input file failed");
            dprintf(client_fd, "Error opening input file '%s': %s\n", cmd->input_file, strerror(errno));
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
            // Don't close pipe_in_fd here, full cleanup below
            exit(EXIT_FAILURE);
        }
        // Don't close pipe_in_fd yet, will be closed in the loop below
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
            // Error message will go to original stderr (client_fd or pipe)
            dprintf(STDERR_FILENO, "Error opening output file '%s': %s\n", cmd->output_file, strerror(errno));
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
            // Don't close pipe_out_fd here yet
            exit(EXIT_FAILURE);
        }
        // Redirect STDERR to client
        if (dup2(client_fd, STDERR_FILENO) == -1) {
             log_perror("dup2 STDERR to client_fd failed");
             // Don't close pipe_out_fd here yet
             exit(EXIT_FAILURE);
        }
        // Don't close pipe_out_fd yet, will be closed in the loop below
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

    // --- Execute Command ---
    log_info("Child (pid %d) executing: %s", getpid(), cmd->argv[0]);
    execvp(cmd->argv[0], cmd->argv);

    // --- execvp Failed ---
    // If execvp returns, an error occurred.
    log_perror("execvp failed");
    // Error message is printed via perror to the (potentially redirected) STDERR.
    dprintf(STDERR_FILENO, "Error executing command '%s': %s\n", cmd->argv[0], strerror(errno));
    exit(EXIT_FAILURE); // Terminate child on execvp failure
}


/**
 * @brief Prints help information to the specified file descriptor.
 */
static void print_help(int output_fd) {
    // Use dprintf for writing directly to the file descriptor
    dprintf(output_fd, "Simple Shell (SPaASM Assignment 2)\n");
    dprintf(output_fd, "Author: [Your Name/ID Here]\n"); // <-- TODO: Update Author Name
    dprintf(output_fd, "Usage: shell [-s|-c] [-p port] [-i bind_addr] [-v] [-l logfile] [-h]\n");
    dprintf(output_fd, "  -s : Run in server mode (default)\n");
    dprintf(output_fd, "  -c : Run in client mode\n");
    dprintf(output_fd, "  -p port : Port number\n");
    dprintf(output_fd, "  -i addr : IP address to bind server to (default: all)\n");
    dprintf(output_fd, "  -v : Enable verbose output to stderr\n");
    dprintf(output_fd, "  -l file : Log output to specified file (default: stderr)\n");
    dprintf(output_fd, "  -h : Display this help message\n\n");
    dprintf(output_fd, "Built-in Commands:\n");
    dprintf(output_fd, "  help : Display this help message.\n");
    dprintf(output_fd, "  quit : Disconnect from the server.\n");
    dprintf(output_fd, "  halt : Terminate the server process.\n\n");
    dprintf(output_fd, "Special Characters:\n");
    dprintf(output_fd, "  # : Start a comment (ignored).\n");
    dprintf(output_fd, "  ; : Separate commands to be executed sequentially.\n");
    dprintf(output_fd, "  | : Pipe the output of one command to the input of the next.\n");
    dprintf(output_fd, "  < file : Redirect standard input from a file.\n");
    dprintf(output_fd, "  > file : Redirect standard output to a file (overwrite).\n");
}