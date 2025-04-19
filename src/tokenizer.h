#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <stddef.h> // For size_t

// Enum defining the different types of tokens
typedef enum {
    TOKEN_WORD,      // Command, argument, filename, etc.
    TOKEN_PIPE,      // |
    TOKEN_REDIR_IN,  // <
    TOKEN_REDIR_OUT, // >
    TOKEN_SEMICOLON, // ;
    TOKEN_COMMENT,   // # (and everything after it on the line)
    TOKEN_EOF,       // End of input string
    TOKEN_ERROR      // Represents a tokenization error (e.g., unclosed quote)
} TokenType;

// Structure representing a single token
typedef struct Token {
    TokenType type;
    char *value;      // Dynamically allocated string for TOKEN_WORD, or NULL for others
    struct Token *next; // Pointer to the next token in a linked list
} Token;

// Function to tokenize an input string.
// Returns a pointer to the head of a linked list of tokens.
// Returns NULL on allocation failure or if the input is fundamentally invalid.
// An ERROR token might be included in the list for syntax errors like unclosed quotes.
Token *tokenize(const char *input);

// Function to free the linked list of tokens
void free_tokens(Token *head);

// Function to print tokens (for debugging)
void print_tokens(Token *head);

#endif // TOKENIZER_H