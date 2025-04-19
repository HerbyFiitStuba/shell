#include "parser.h"
#include "tokenizer.h" // Included via parser.h, but good practice
#include <stdio.h>     // For printf, fprintf, stderr
#include <stdlib.h>    // For malloc, realloc, free
#include <string.h>    // For strcmp, strdup
#include <stdbool.h>   // For bool type

// --- Forward Declarations for Helper Functions ---
static Command *create_command();
static int add_argument(Command *cmd, const char *arg);
static void free_single_command(Command *cmd); // Helper for freeing one command node
static const char* token_type_to_string(TokenType type); // Helper for error messages


// Parses the token list and builds the command structure.
// Returns the head of the command sequence list, or NULL on error.
Command *parse(Token *tokens) {
    // --- Initial checks ---
    if (!tokens || tokens->type == TOKEN_EOF) {
        return NULL; // Nothing to parse
    }
    if (tokens->type == TOKEN_ERROR) {
        fprintf(stderr, "Parser error: Tokenizer reported error: %s\n", tokens->value ? tokens->value : "Unknown error");
        return NULL; // Tokenizer error at the start
    }

    Command *head_sequence = NULL;
    Command *tail_sequence = NULL;

    Command *current_pipeline_head = NULL;
    Command *current_pipeline_tail = NULL;
    Command *last_sequence_pipeline_tail = NULL; // For linking pipelines to sequences

    Command *current_cmd = NULL;

    Token *current_token = tokens;
    Token *prev_token = NULL;
    bool error_occurred = false; // Flag to track errors

    while (current_token != NULL && current_token->type != TOKEN_EOF && current_token->type != TOKEN_ERROR) {

        // --- Create new command if needed ---
        if (current_cmd == NULL) {
            // Don't create a command if the previous token was ';' which implies an empty command segment.
            // The semicolon handler below manages the state correctly. Only create if not immediately after ';'.
            // Exception: If pipeline is also NULL, we are starting fresh.
            if (!prev_token || prev_token->type != TOKEN_SEMICOLON || !current_pipeline_head) {
                 current_cmd = create_command();
                 if (!current_cmd) {
                     error_occurred = true; // Allocation failed
                     break; // Break while loop
                 }

                 // Link into pipeline
                 if (current_pipeline_head == NULL) {
                     current_pipeline_head = current_cmd;
                     current_pipeline_tail = current_cmd;
                 } else {
                      // This should only happen after a pipe '|'
                      if (prev_token && prev_token->type == TOKEN_PIPE) {
                          current_pipeline_tail->next_command_in_pipeline = current_cmd;
                          current_pipeline_tail = current_cmd;
                      } else {
                          // Should not happen with correct logic (e.g., after ';')
                          fprintf(stderr, "Syntax error: Unexpected state, attempting to create new command without preceding pipe in an active pipeline.\n");
                          error_occurred = true;
                          break; // Break while loop
                      }
                 }
            }
        }

        // --- Syntax Checks (Basic) ---
        TokenType current_type = current_token->type;
        TokenType prev_type = prev_token ? prev_token->type : TOKEN_EOF; // Treat start as EOF

        // Check for consecutive operators (;, |)
        if ((prev_type == TOKEN_SEMICOLON || prev_type == TOKEN_PIPE) &&
            (current_type == TOKEN_SEMICOLON || current_type == TOKEN_PIPE)) {
            fprintf(stderr, "Syntax error: Unexpected token '%s' after '%s'\n",
                    token_type_to_string(current_type), token_type_to_string(prev_type));
            error_occurred = true;
            break; // Break while loop
        }
        // Check for operator at the very beginning
         if (prev_token == NULL && (current_type == TOKEN_SEMICOLON || current_type == TOKEN_PIPE)) {
             fprintf(stderr, "Syntax error: Unexpected token '%s' at beginning of command\n", token_type_to_string(current_type));
             error_occurred = true;
             break; // Break while loop
         }


        // --- Handle Token Types ---
        switch (current_type) {
            case TOKEN_WORD:
                 // Ensure a command structure exists to add the word to.
                 // This handles cases like ` ; word` or `| word` if create_command logic changes.
                 if (!current_cmd) {
                     fprintf(stderr, "Syntax error: Unexpected word '%s' without preceding command structure.\n", current_token->value);
                     error_occurred = true;
                     break; // Break switch
                 }
                if (add_argument(current_cmd, current_token->value) != 0) {
                    // Allocation error in add_argument or strdup (perror already called)
                    error_occurred = true;
                    // Break switch, error flag will break while loop below
                }
                break;

            case TOKEN_REDIR_IN: // <
            case TOKEN_REDIR_OUT: // >
                {
                    // Syntax Check: Next token must be a WORD (filename)
                    Token *filename_token = current_token->next;
                    if (!filename_token || filename_token->type != TOKEN_WORD) {
                        fprintf(stderr, "Syntax error: Expected filename after '%s'\n", token_type_to_string(current_type));
                        error_occurred = true;
                        break; // Break switch
                    }
                    // Syntax Check: Ensure command exists to apply redirection to
                    if (!current_cmd || !current_cmd->argv || !current_cmd->argv[0]) {  // No command yet
                        // This catches `| < file`, `; < file`, `< file ;`, `> file ;` and start ` < file`
                         fprintf(stderr, "Syntax error: Redirection '%s' without a preceding command.\n", token_type_to_string(current_type));
                         error_occurred = true;
                         break; // Break switch
                    }

                    // Set the appropriate redirection field
                    char **target_field = (current_type == TOKEN_REDIR_IN) ? &(current_cmd->input_file) : &(current_cmd->output_file);

                    // Syntax Check: Ensure redirection is not already set
                    if (*target_field != NULL) {
                        fprintf(stderr, "Syntax error: Multiple %s redirections for the same command.\n",
                                (current_type == TOKEN_REDIR_IN) ? "input" : "output");
                        error_occurred = true;
                        break; // Break switch
                    }

                    // Assign filename
                    *target_field = strdup(filename_token->value);
                    if (!*target_field) {
                        perror("Failed to duplicate redirection filename");
                        error_occurred = true;
                        break; // Break switch
                    }

                    // Consume the filename token as well
                    prev_token = filename_token; // Update prev_token since we are skipping one
                    current_token = filename_token; // Advance current_token past the filename
                }
                break; // Break case TOKEN_REDIR_IN/OUT

            case TOKEN_PIPE: // |
                // Syntax Check: Ensure there is a command before the pipe
                if (!current_cmd || !current_cmd->argv || !current_cmd->argv[0]) {
                     fprintf(stderr, "Syntax error: Unexpected pipe '|' without a preceding command.\n");
                     error_occurred = true;
                     break; // Break switch
                }
                 // Syntax Check: Ensure something follows the pipe
                 if (!current_token->next || current_token->next->type != TOKEN_WORD) {
                    // This catches `cmd | ;`, `cmd | < file`, `cmd | > file`
                     fprintf(stderr, "Syntax error: Unexpected end of input or operator after pipe '|'.\n");
                     error_occurred = true;
                     break; // Break switch
                 }

                // Current command is finished, ready for the next in the pipeline
                current_cmd = NULL;
                break; // Break case TOKEN_PIPE

            case TOKEN_SEMICOLON: // ;
                 // Syntax Check: Ensure there was a command before the semicolon, unless the previous token was also a semicolon.
                 if (!current_cmd || !current_cmd->argv || !current_cmd->argv[0]) {
                      // This catches `| ;`, `< file ;`, `> file ;` and start ` ; cmd`
                      if (prev_type != TOKEN_SEMICOLON) { // Allow `;;` by ignoring the second one if the first was valid.
                           fprintf(stderr, "Syntax error: Unexpected semicolon ';' without a preceding valid command.\n");
                           error_occurred = true;
                           break; // Break switch
                      }
                      // If prev_type was ';', effectively ignore the current ';'. current_cmd remains NULL.
                 }

                 // If the command preceding ';' was valid, link the pipeline.
                 if (current_cmd && current_cmd->argv && current_cmd->argv[0]) {
                     // Syntax Check: Ensure something valid follows the semicolon (unless it's the end)
                     if (current_token->next && (current_token->next->type == TOKEN_SEMICOLON || current_token->next->type == TOKEN_PIPE)) {
                          fprintf(stderr, "Syntax error: Unexpected operator '%s' after semicolon ';'.\n", token_type_to_string(current_token->next->type));
                          error_occurred = true;
                          break; // Break switch
                     }

                    // Link the completed pipeline to the sequence list
                    if (current_pipeline_head) { // Should always be true if current_cmd is valid
                        if (head_sequence == NULL) {
                            head_sequence = current_pipeline_head;
                            tail_sequence = current_pipeline_head;
                        } else {
                            // Ensure tail_sequence is valid before dereferencing
                            if (tail_sequence) {
                                tail_sequence->next_command_sequence = current_pipeline_head;
                                //tail_sequence = current_pipeline_tail; // Tail of sequence is tail of last pipeline added
                                //should be current_pipeline_head
                                tail_sequence = current_pipeline_head;
                                if (last_sequence_pipeline_tail) {
                                    last_sequence_pipeline_tail->next_command_sequence = current_pipeline_head;
                                } else{
                                    // This should not happen if the logic is correct
                                    fprintf(stderr, "Internal parser error: last_sequence_pipeline_tail is NULL when linking.\n");
                                    error_occurred = true;
                                    break; // Break switch
                                }
                            } else {
                                // Should not happen if head_sequence is not NULL
                                fprintf(stderr, "Internal parser error: head_sequence exists but tail_sequence is NULL during linking.\n");
                                error_occurred = true;
                                break; // Break switch
                            }
                        }
                        // Reset for the next pipeline/command
                        last_sequence_pipeline_tail = current_pipeline_tail; // Save the last pipeline tail for linking
                        current_pipeline_head = NULL;
                        current_pipeline_tail = NULL;
                    } else {
                         // Should not happen if current_cmd is valid
                         fprintf(stderr, "Internal parser error: Valid command found but no pipeline head.\n");
                         error_occurred = true;
                         break; // Break switch
                    }
                 }
                 // Else (command before ';' was invalid or empty ';'), don't link anything.

                // Reset current_cmd regardless, ready for the next sequence item or error handling.
                current_cmd = NULL;
                break; // Break case TOKEN_SEMICOLON

            // TOKEN_EOF and TOKEN_ERROR are handled by the loop condition
            default:
                // Should not happen with defined token types
                fprintf(stderr, "Internal parser error: Unknown token type %d\n", current_type);
                error_occurred = true;
                // Break switch
                break;
        } // End switch

        // If an error occurred inside the switch or basic syntax checks, break the outer while loop
        if (error_occurred) {
            break;
        }

        prev_token = current_token;
        current_token = current_token->next;
    } // End while loop

    // --- Post-Loop Checks and Cleanup ---

    // Check if loop terminated due to tokenizer error token
    if (!error_occurred && current_token && current_token->type == TOKEN_ERROR) {
         fprintf(stderr, "Parser error: Tokenizer reported error: %s\n", current_token->value ? current_token->value : "Unknown error");
         error_occurred = true;
    }

    // Perform final syntax checks only if no error has occurred yet
    if (!error_occurred) {
        // Check for trailing operators that require arguments
        if (prev_token && (prev_token->type == TOKEN_PIPE || prev_token->type == TOKEN_REDIR_IN || prev_token->type == TOKEN_REDIR_OUT)) {
             fprintf(stderr, "Syntax error: Unexpected end of input after operator '%s'\n", token_type_to_string(prev_token->type));
             error_occurred = true;
        }
        // Check if the last command in the potentially unlinked pipeline was valid
        else if (current_pipeline_head && // A pipeline was started...
            (!current_pipeline_tail || !current_pipeline_tail->argv || !current_pipeline_tail->argv[0])) {
             // ...but the last command added to it is empty/invalid (e.g., `cmd |`, `cmd < file`)
             fprintf(stderr, "Syntax error: Incomplete command at end of input.\n");
             error_occurred = true;
        }
    }


    // --- Cleanup on Error OR Finalize ---
    if (error_occurred) {
        // Determine if current_pipeline_head needs separate freeing.
        // This is needed if it exists and is not the start of any sequence
        // already linked into head_sequence.
        bool free_current_pipeline_separately = false;
        if (current_pipeline_head) {
            bool linked_to_head = false;
            Command* temp = head_sequence;
            while (temp) {
                if (temp == current_pipeline_head) {
                    linked_to_head = true;
                    break;
                }
                temp = temp->next_command_sequence;
            }
            if (!linked_to_head) {
                free_current_pipeline_separately = true;
            }
        }

        // Perform the frees
        free_command_sequence(head_sequence); // Free the main linked list of sequences/pipelines
        if (free_current_pipeline_separately) {
            free_command_sequence(current_pipeline_head); // Free the unlinked pipeline part
        }

        return NULL; // Indicate failure
    } else {
        // --- Success Path ---
        // Link the last pipeline (if any) to the sequence list
        if (current_pipeline_head) {
            if (head_sequence == NULL) {
                head_sequence = current_pipeline_head;
                // tail_sequence is already current_pipeline_head
            } else {
                // Ensure tail_sequence is valid before dereferencing
                if (tail_sequence) {
                    tail_sequence->next_command_sequence = current_pipeline_head;  // Link the last pipeline 
                    // No need to update tail_sequence, we are at the end.
                } else {
                    // Should not happen if head_sequence is not NULL
                    fprintf(stderr, "Internal parser error: head_sequence exists but tail_sequence is NULL.\n");
                    free_command_sequence(head_sequence); // Clean up what we have
                    free_command_sequence(current_pipeline_head); // Clean up current pipeline too
                    return NULL;
                }
            }
        }
        // else: No final pipeline to link (e.g., input ended with ';')

        // Success
        return head_sequence;
    }
}


// --- Helper Functions Implementation ---

// Helper to get string representation of token type for errors
static const char* token_type_to_string(TokenType type) {
    switch (type) {
        case TOKEN_WORD: return "WORD";
        case TOKEN_PIPE: return "|";
        case TOKEN_REDIR_IN: return "<";
        case TOKEN_REDIR_OUT: return ">";
        case TOKEN_SEMICOLON: return ";";
        case TOKEN_COMMENT: return "#";
        case TOKEN_EOF: return "EOF";
        case TOKEN_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// Allocates and initializes a new Command struct. Returns NULL on failure.
static Command *create_command() {
    Command *cmd = (Command *)malloc(sizeof(Command));
    if (!cmd) {
        perror("Failed to allocate memory for command");
        return NULL;
    }
    cmd->argv = NULL;
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->next_command_in_pipeline = NULL;
    cmd->next_command_sequence = NULL;

    // Initialize argv with space for the NULL terminator
    cmd->argv = (char **)malloc(sizeof(char *));
    if (!cmd->argv) {
        perror("Failed to allocate memory for argv");
        free(cmd);
        return NULL;
    }
    cmd->argv[0] = NULL; // Start with a NULL-terminated list

    return cmd;
}

// Adds an argument to the command's argv list. Handles reallocation.
// Returns 0 on success, -1 on failure.
static int add_argument(Command *cmd, const char *arg) {
    if (!cmd || !arg) return -1; // Invalid input

    // Count current arguments
    int argc = 0;
    if (cmd->argv) {
        while (cmd->argv[argc] != NULL) {
            argc++;
        }
    }

    // Reallocate argv array: need space for argc elements + new arg + NULL terminator
    char **new_argv = (char **)realloc(cmd->argv, (argc + 2) * sizeof(char *));
    if (!new_argv) {
        perror("Failed to reallocate argv");
        // Original cmd->argv is still valid but not resized
        return -1;
    }
    cmd->argv = new_argv;

    // Duplicate the argument string
    cmd->argv[argc] = strdup(arg);
    if (!cmd->argv[argc]) {
        perror("Failed to duplicate argument string");
        // Need to decide how to handle this - shrink argv back? Or just fail?
        // For now, just mark failure. The caller might need to clean up.
        cmd->argv[argc] = NULL; // Keep it NULL terminated
        return -1;
    }

    // Add the NULL terminator
    cmd->argv[argc + 1] = NULL;

    return 0;
}


// Frees memory associated with a single Command struct node
// Does NOT free next_command_in_pipeline or next_command_sequence
static void free_single_command(Command *cmd) {
    if (!cmd) return;

    // Free the argv array and the strings within it
    if (cmd->argv) {
        for (int i = 0; cmd->argv[i] != NULL; i++) {
            free(cmd->argv[i]); // Free each duplicated argument string
        }
        free(cmd->argv); // Free the array itself
    }

    // Free redirection filenames
    free(cmd->input_file);
    free(cmd->output_file);

    // Free the command struct itself
    free(cmd);
}

// Recursively frees the entire command sequence, including pipelines.
void free_command_sequence(Command *cmd_sequence) {
    Command *current_seq = cmd_sequence;
    while (current_seq != NULL) {
        Command *next_seq = current_seq->next_command_sequence; // Store next sequence link

        // Free the entire pipeline starting from current_seq head node
        Command *current_pipe = current_seq;
        while (current_pipe != NULL) {
             Command *next_pipe = current_pipe->next_command_in_pipeline; // Store next pipeline link
             // Null out links before freeing to potentially help prevent issues,
             // though the parse logic aims to avoid double free scenarios.
             current_pipe->next_command_sequence = NULL;
             current_pipe->next_command_in_pipeline = NULL;
             free_single_command(current_pipe); // Free the current command node in the pipeline
             current_pipe = next_pipe;
        }
        current_seq = next_seq; // Move to the next command sequence
    }
}


// --- Debug Print Function ---

// Prints the command structure for debugging.
void print_command_sequence(Command *cmd_sequence) {
    printf("--- Parsed Command Sequence ---\n");
    int seq_num = 0;
    Command *current_seq = cmd_sequence;
    while (current_seq != NULL) {
        printf("Sequence %d:\n", seq_num++);
        int pipe_num = 0;
        Command *current_pipe = current_seq;
        while (current_pipe != NULL) {
            printf("  Pipeline Stage %d:\n", pipe_num++);
            printf("    Command: ");
            if (current_pipe->argv && current_pipe->argv[0]) {
                printf("\"%s\"\n", current_pipe->argv[0]);
                printf("    Args: [");
                for (int i = 0; current_pipe->argv[i] != NULL; i++) {
                    printf("\"%s\"%s", current_pipe->argv[i], current_pipe->argv[i+1] ? ", " : "");
                }
                printf("]\n");
            } else {
                printf("(null or empty)\n");
            }
            printf("    Input Redir: %s\n", current_pipe->input_file ? current_pipe->input_file : "(none)");
            printf("    Output Redir: %s\n", current_pipe->output_file ? current_pipe->output_file : "(none)");

            current_pipe = current_pipe->next_command_in_pipeline;
             if (current_pipe) printf("    |\n"); // Indicate pipe connection visually
        }
         printf("  ---\n");
        current_seq = current_seq->next_command_sequence;
         if (current_seq) printf(";\n"); // Indicate sequence connection visually
    }
     printf("--- End Sequence ---\n");
}