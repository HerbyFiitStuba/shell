// filepath: /home/matej/shell/src/test_parser.c
#include "parser.h"
#include "tokenizer.h"
#include <stdio.h>
#include <stdlib.h> // For NULL

// Function to run a single parser test case
void run_parser_test(const char *input) {
    printf("--- Testing Input: \"%s\" ---\n", input);

    // 1. Tokenize the input
    Token *tokens = tokenize(input);
    if (tokens == NULL) {
        // Handle cases where tokenizer itself might fail or return nothing (e.g., unclosed quotes)
        // The tokenizer might print its own error.
        printf("Tokenizer failed or returned NULL for input.\n");
        printf("-------------------------------------\n\n");
        return;
    }
    // Optional: Print tokens for debugging the test itself
    // printf("Tokens generated:\n");
    // print_tokens(tokens);

    // 2. Parse the tokens
    Command *cmd_sequence = parse(tokens);

    // 3. Check results and print/cleanup
    if (cmd_sequence == NULL) {
        // Parser returns NULL on syntax error or allocation failure
        // Parser function should have printed an error message to stderr
        printf("Parser returned NULL (Syntax Error or Allocation Failure detected).\n");
    } else {
        printf("Parser Succeeded. Parsed Structure:\n");
        print_command_sequence(cmd_sequence); // Use the print function from parser.c
        free_command_sequence(cmd_sequence);  // Clean up command structure
    }

    // 4. Clean up tokens
    free_tokens(tokens);

    printf("-------------------------------------\n\n");
}

int main() {
    printf("Starting Parser Tests...\n\n");

       // --- Define Test Cases ---
       const char *test_inputs[] = {
        // --- Basic Valid Cases ---
        "ls",                                       // Simplest command
        "ls -l /home",                              // Simple command with args
        "echo hello world",                         // Command with multiple args
        "  ls   -l   ;  pwd   ",                    // Extra whitespace
        "cmd # comment here",                       // Command with comment
        "echo 'hello ; world' | cat",               // Quoted semicolon
        "echo \"hello | world\" ; pwd",             // Quoted pipe
        "ls > out.txt < in.txt",                    // Valid redirection order
        "ls ;",                                     // Semicolon at end (allowed)
        "cmd1 ;",                                   // Trailing semicolon (allowed)

        // --- Pipeline Valid Cases ---
        "cat file.txt | grep 'search term' | sort", // Basic pipeline
        "cmd1 | cmd2 | cmd3 | cmd4",                // Longer pipeline
        "cmd1 arg1 | cmd2 arg2 arg3 | cmd3",        // Pipeline with args

        // --- Redirection Valid Cases ---
        "cmd1 < input.txt > output.txt",            // Simple redirections
        "cmd1 arg1 < input.txt arg2 > output.txt arg3", // Redirections mixed with args
        "cmd1 <in.txt>out.txt",                      // Redirections without spaces (allowed)
        "cmd1 < in.txt > out.txt",                  // Redirections with spaces
        "cmd1 > out.txt < in.txt",                  // Mixed order of redirections
        "cmd1 < file ;",                            // Sequence ending after redirection

        // --- Sequence Valid Cases ---
        "cmd1 ; cmd2 arg1 arg2 ; cmd3",             // Basic sequence
        "cmd1 ; cmd2 ; cmd3 ; cmd4",                // Longer sequence


        // --- Mixed Valid Cases ---
        "cmd1 | cmd2 < input.txt | cmd3 > output.txt", // Pipeline with redirections
        "cmd1 < in.txt ; cmd2 | cmd3 > out.txt ; cmd4", // Mixed sequence and pipeline
        "cmd1 > out1 ; cmd2 < in2 | cmd3 > out3 ; cmd4 < in4", // More complex mix
        "cmd1 < in1 | cmd2 > out2 ; cmd3 < in3 | cmd4 > out4", // Sequence of pipelines
        "cmd1 ; cmd2 | cmd3 ; cmd4",                // Sequence containing a pipeline
        "cmd1 | cmd2 ; cmd3 | cmd4",                // Pipeline containing a sequence point
        "cmd1 < file ; cmd2 | cmd3 > file2 ; cmd4 # comment", // Mix with comment
        "cmd1 > file ;",
        "cmd1 < file cmd2",                         // Command word after redirection filename (will be treated as an arg) 
                                                    // not an error but to the reader who got here 
                                                    // !!! do not use this syntax please !!!
        "echo hello\\",                             // Dangling escape (this one we handle as valid)
        // tokenizer does not support \ escaping as of this version

        // --- Empty/Whitespace/Comment Cases ---
        "",                                         // Empty input
        " ",                                        // Whitespace only
        "\t",                                       // Tab only
        "# comment line",                           // Comment only
        "  # comment line",                         // Comment with leading space
        ";",                                        // Just a semicolon
        "|",                                        // Just a pipe
        "<",                                        // Just redirection
        ">",                                        // Just redirection

        // --- Basic Invalid Cases ---
        "| ls",                                     // Pipe at start
        "ls |",                                     // Pipe at end
        "; ls",                                     // Semicolon at start (not allowed)
        "ls || grep",                               // Double pipe
        "ls ;; pwd",                                // Double semicolon (not allowed)
        "ls <",                                     // Missing input filename
        "ls >",                                     // Missing output filename
        "< input.txt",                              // Input redirection without command
        "> output.txt",                             // Output redirection without command
        "ls < file1 < file2",                       // Multiple input redirections
        "ls > file1 > file2",                       // Multiple output redirections
        "ls | ; pwd",                               // Pipe followed by semicolon
        "ls ; | pwd",                               // Semicolon followed by pipe
        "ls < > out.txt",                           // Adjacent redirections (invalid)
        "<in.txt cmd1 >out.txt",                    // Redirections before command (less common style)
        "; cmd1",                                   // Leading semicolon (often disallowed, check behavior)
        "cmd1 ;; cmd2",                             // Double semicolon (often treated as one)

        // --- More Complex Invalid Cases ---
        "cmd1 < | cmd2",                            // Redirection followed by pipe
        "cmd1 > | cmd2",                            // Redirection followed by pipe
        "cmd1 < ; cmd2",                            // Redirection followed by semicolon
        "cmd1 > ; cmd2",                            // Redirection followed by semicolon
        "cmd1 | < file",                            // Pipe followed by input redirection
        "cmd1 | > file",                            // Pipe followed by output redirection
        "cmd1 ; < file",                            // Semicolon followed by input redirection
        "cmd1 ; > file",                            // Semicolon followed by output redirection
        "cmd1 < file |",                            // Pipeline ending after redirection
        "cmd1 | cmd2 <",                            // Missing filename in pipeline
        "cmd1 | cmd2 >",                            // Missing filename in pipeline
        "cmd1 ; cmd2 <",                            // Missing filename in sequence
        "cmd1 ; cmd2 >",                            // Missing filename in sequence

        "cmd1 | | cmd2",                            // Double pipe variation
        "cmd1 ; ; cmd2",                            // Double semicolon variation

        // --- Tokenizer Errors ---
        "echo \"unclosed quote",                    // Unclosed double quote
        "echo \'unclosed quote",                    // Unclosed single quote
        NULL                                        // Sentinel to mark the end
    };

    // --- Run Tests ---
    for (int i = 0; test_inputs[i] != NULL; i++) {
        run_parser_test(test_inputs[i]);
    }

    printf("Parser Tests Finished.\n");
    return 0;
}