// filepath: /home/matej/shell/src/test_tokenizer.c
#include "tokenizer.h"
#include <stdio.h>
#include <string.h>

// Function to run a single test case
void run_test(const char *input) {
    printf("--- Testing Input: \"%s\" ---\n", input);
    Token *tokens = tokenize(input);

    if (tokens == NULL) {
        printf("Tokenizer returned NULL (likely an allocation error or syntax error like unclosed quote).\n");
    } else {
        print_tokens(tokens); // Use the existing print function
        free_tokens(tokens);  // Clean up memory
    }
    printf("-------------------------------------\n\n");
}

int main() {
    printf("Starting Tokenizer Tests...\n\n");

    // --- Define Test Cases ---
    const char *test_inputs[] = {
        "ls -l /home",                     // Simple command
        "  cd   /tmp ;   pwd  ",           // Whitespace and semicolon
        "echo hello > output.txt",         // Output redirection
        "sort < input.log",                // Input redirection
        "cat file.txt | grep 'search term'", // Pipe and single quotes
        "command \"arg with spaces\" ; next", // Double quotes and semicolon
        "echo 'nested \"double\" quotes'",   // Nested quotes 1
        "echo \"nested 'single' quotes\"",   // Nested quotes 2
        "ls # This is a comment",          // Comment at end
        "# Whole line comment",            // Comment line
        "cmd1 ; # comment after semicolon", // Comment after operator
        "cmd1 < in | cmd2 > out ; cmd3",   // Mixed operators
        "cmd1<in|cmd2>out;cmd3",           // Mixed operators
        "",                                // Empty input
        " ",                               // Just whitespace
        "single'quote'word",               // Quotes within word (becomes one word)
        "word\"with\"quote",               // Quotes within word
        "echo 'unclosed single quote",     // Error: Unclosed single quote
        "echo \"unclosed double quote",    // Error: Unclosed double quote
        "|",                               // Operator only
        "< > ; |",                         // Multiple operators
        "<>;|",                            // Multiple operators
        ";",                               // Semicolon only
        ";;",                              // Multiple semicolons
        NULL                               // Sentinel to mark the end
    };

    // --- Run Tests ---
    for (int i = 0; test_inputs[i] != NULL; i++) {
        run_test(test_inputs[i]);
    }

    printf("Tokenizer Tests Finished.\n");

    return 0;
}