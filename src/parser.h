#ifndef PARSER_H
#define PARSER_H

#include "tokenizer.h" // Need Token definition

// Structure to represent a single command or a stage in a pipeline
typedef struct Command {
    char **argv;             // Array of arguments (argv[0] is the command).
                             // Must be NULL-terminated. Dynamically allocated.
    char *input_file;        // Filename for input redirection (<), or NULL. Dynamically allocated.
    char *output_file;       // Filename for output redirection (>), or NULL. Dynamically allocated.

    struct Command *next_command_in_pipeline; // Next command connected by '|', or NULL
    struct Command *next_command_sequence;  // Next command separated by ';', or NULL
} Command;

// Function to parse a sequence of tokens into a linked list of Command structures.
// The list represents commands separated by semicolons (using next_command_sequence).
// Pipelines are represented within this structure using the next_command_in_pipeline pointer.
// Returns the head of the command sequence list, or NULL on error (syntax or allocation).
Command *parse(Token *tokens);

// Function to free the memory allocated for the command structures.
// Handles freeing argv arrays, filenames, and the structs themselves,
// including pipelines and sequences.
void free_command_sequence(Command *cmd_sequence);

// Function to print the parsed command structure (for debugging)
void print_command_sequence(Command *cmd_sequence);


#endif // PARSER_H