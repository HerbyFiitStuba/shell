#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "parser.h" // For the Command structure definition

// Define return codes for execute_command_sequence
#define EXEC_OK      0  // Sequence completed successfully
#define EXEC_ERROR  -1  // Critical error (e.g., fork failed, major setup issue)
#define EXEC_HALT    1  // 'halt' command was executed, server should terminate
#define EXEC_QUIT    2  // 'quit' command was executed, client connection should close

/**
 * @brief Executes a parsed command sequence received from a client.
 *
 * This function iterates through the command sequence (commands separated by ';').
 * For each command (which might be a pipeline), it checks for built-in commands
 * or executes external commands, handling pipelines ('|') and I/O redirection ('<', '>').
 * Standard input/output/error for executed commands, if not redirected by files,
 * should be directed to/from the client_fd.
 *
 * @param cmd_sequence The head of the command sequence list returned by parse().
 * @param client_fd The file descriptor representing the connection to the client.
 *                  This is used for communication (reading commands is done elsewhere,
 *                  this is for sending output/errors back) and for the 'quit' command.
 * @return int Returns one of the EXEC_* codes:
 *             EXEC_OK: The entire sequence finished normally.
 *             EXEC_ERROR: A non-recoverable error occurred during execution setup (e.g., fork).
 *             EXEC_HALT: The 'halt' built-in was encountered.
 *             EXEC_QUIT: The 'quit' built-in was encountered for this client.
 */
int execute_command_sequence(Command *cmd_sequence, int client_fd);

#endif // EXECUTOR_H