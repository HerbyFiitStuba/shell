#include "tokenizer.h"
#include <stdlib.h> // For malloc, realloc, free
#include <string.h> // For strdup, strlen, strcmp
#include <stdio.h>  // For printf (in print_tokens)
#include <ctype.h>  // For isspace

// --- Helper Functions ---

// Creates a new token. Returns NULL on allocation failure.
// Note: For TOKEN_WORD, 'value' is duplicated using strdup.
//       For other types, 'value' is ignored (token->value will be NULL).
static Token *create_token(TokenType type, const char *value) {
    Token *token = (Token *)malloc(sizeof(Token));
    if (!token) {
        perror("Failed to allocate memory for token");
        return NULL;
    }
    token->type = type;
    token->next = NULL;
    token->value = NULL;

    if (type == TOKEN_WORD && value != NULL) {
        token->value = strdup(value);
        if (!token->value) {
            perror("Failed to duplicate token value");
            free(token);
            return NULL;
        }
    } else if (type == TOKEN_ERROR && value != NULL) {
        // Allow storing an error message
        token->value = strdup(value);
         if (!token->value) {
            perror("Failed to duplicate error token value");
            free(token);
            return NULL;
        }
    }

    return token;
}

// Appends a character 'c' to a dynamically resizing string buffer.
//
// Parameters:
//   buffer: Pointer to the char pointer holding the buffer. This will be updated
//           if reallocation occurs.
//   len: Pointer to the current length of the string in the buffer (excluding null terminator).
//        This will be incremented.
//   capacity: Pointer to the currently allocated size of the buffer.
//             This will be updated if reallocation occurs.
//   c: The character to append.
//
// Returns:
//   0 on success.
//  -1 on memory allocation failure (realloc failed).
static int append_char(char **buffer, size_t *len, size_t *capacity, char c) {
    const size_t initial_capacity = 16;

    // We need space for the current length + the new character + the null terminator.
    if (*len + 1 >= *capacity) {
        // Calculate the new capacity. Double the current capacity,
        // or use the initial capacity if the buffer hasn't been allocated yet.
        size_t new_capacity = (*capacity == 0) ? initial_capacity : *capacity * 2;

        // Attempt to resize the buffer.
        char *new_buffer = realloc(*buffer, new_capacity);
        if (!new_buffer) {
            perror("Failed to realloc word buffer");

            // Return an error code; the caller should handle the cleanup.
            return -1;
        }

        *buffer = new_buffer;
        *capacity = new_capacity;
    }

    (*buffer)[*len] = c;
    (*len)++;
    (*buffer)[*len] = '\0';

    return 0;
}

// Appends a token to the end of the list.
static void append_token(Token **head, Token **tail, Token *new_token) {
    if (!new_token) return; // Should not happen if create_token is checked
    if (*head == NULL) {
        *head = new_token;
        *tail = new_token;
    } else {
        (*tail)->next = new_token;
        *tail = new_token;
    }
}

// --- Tokenizer Core ---

Token *tokenize(const char *input) {
    Token *head = NULL;
    Token *tail = NULL;
    const char *current = input;

    char *word_buffer = NULL;
    size_t buffer_len = 0;
    size_t buffer_capacity = 0;

    typedef enum { 
        STATE_NORMAL, // Normal state, outside quotes
        STATE_IN_SQ,  // Inside single quotes
        STATE_IN_DQ   // Inside double quotes
    } TokenizerState;

    TokenizerState state = STATE_NORMAL;
    char quote_char = 0; // Stores the type of quote (' or ") we are inside

    while (1) { // Loop until explicitly broken or EOF token is added
        char c = *current;

        // --- State: Inside Quotes ---
        if (state == STATE_IN_SQ || state == STATE_IN_DQ) {
            if (c == '\0') {
                // Unclosed quote error
                append_token(&head, &tail, create_token(TOKEN_ERROR, "Unclosed quote"));
                goto cleanup_error; // Use goto for centralized cleanup on error
            } else if (c == quote_char) {
                // Closing quote found
                // Finalize the word token (even if empty)
                Token *word_token = create_token(TOKEN_WORD, word_buffer ? word_buffer : "");  // 
                if (!word_token) goto cleanup_error;
                append_token(&head, &tail, word_token);

                // Reset buffer and state
                buffer_len = 0;
                if (word_buffer) word_buffer[0] = '\0'; // Reset buffer content
                state = STATE_NORMAL;
                quote_char = 0;
                current++; // Consume the closing quote
            } else {
                // Append character within quotes
                if (append_char(&word_buffer, &buffer_len, &buffer_capacity, c) != 0) {
                    goto cleanup_error;
                }
                current++;
            }
        }
        // --- State: Normal ---
        else { // state == STATE_NORMAL
            // Finalize previous word if buffer has content and delimiter is found
            if (buffer_len > 0 && (isspace(c) || strchr("|<>#;", c) || c == '\'' || c == '"' || c == '\0')) {
                 Token *word_token = create_token(TOKEN_WORD, word_buffer);
                 if (!word_token) goto cleanup_error;
                 append_token(&head, &tail, word_token);
                 buffer_len = 0;
                 if (word_buffer) word_buffer[0] = '\0'; // Reset buffer content
            }

            // Handle current character
            if (c == '\0') {
                break; // End of input string
            } else if (isspace(c)) {
                current++; // Skip whitespace
            } else if (c == '|') {
                append_token(&head, &tail, create_token(TOKEN_PIPE, NULL));
                current++;
            } else if (c == '<') {
                append_token(&head, &tail, create_token(TOKEN_REDIR_IN, NULL));
                current++;
            } else if (c == '>') {
                append_token(&head, &tail, create_token(TOKEN_REDIR_OUT, NULL));
                current++;
            } else if (c == ';') {
                append_token(&head, &tail, create_token(TOKEN_SEMICOLON, NULL));
                current++;
            } else if (c == '#') {
                // Comment: Ignore the rest of the line
                // We don't add a COMMENT token, just stop processing
                goto end_tokenize; // Skip adding EOF, just finish
            } else if (c == '\'' || c == '"') {
                // Start of quote
                state = (c == '\'') ? STATE_IN_SQ : STATE_IN_DQ;
                quote_char = c;
                current++; // Consume the opening quote
            } else {
                // Start or continuation of a word
                if (append_char(&word_buffer, &buffer_len, &buffer_capacity, c) != 0) {
                    goto cleanup_error;
                }
                current++;
            }
        }
    } // End while(1)

    // Check for unclosed quotes if loop exited normally (shouldn't happen with current logic, but good practice)
    if (state != STATE_NORMAL) {
         append_token(&head, &tail, create_token(TOKEN_ERROR, "Unclosed quote at end of input"));
         goto cleanup_error;
    }

end_tokenize:
    // Append EOF token
    append_token(&head, &tail, create_token(TOKEN_EOF, NULL));
    free(word_buffer); // Free the reusable buffer
    return head;

cleanup_error:
    // Centralized cleanup on memory allocation or syntax error
    fprintf(stderr, "Tokenizer error occurred.\n");
    free_tokens(head); // Free any tokens created so far
    free(word_buffer); // Free the buffer
    return NULL;       // Indicate failure
}

// --- Utility Functions ---

// Frees the memory allocated for a linked list of tokens
void free_tokens(Token *head) {
    Token *current = head;
    Token *next;
    while (current != NULL) {
        next = current->next;
        free(current->value); // Free the duplicated string value
        free(current);        // Free the token struct itself
        current = next;
    }
}

// Prints tokens for debugging purposes
void print_tokens(Token *head) {
    printf("Tokens:\n");
    Token *current = head;
    while (current != NULL) {
        printf("  Type: %d", current->type);
        switch (current->type) {
            case TOKEN_WORD:      printf(" (WORD)     , Value: \"%s\"\n", current->value ? current->value : "(null)"); break;
            case TOKEN_PIPE:      printf(" (PIPE)     , Value: |\n"); break;
            case TOKEN_REDIR_IN:  printf(" (REDIR_IN) , Value: <\n"); break;
            case TOKEN_REDIR_OUT: printf(" (REDIR_OUT), Value: >\n"); break;
            case TOKEN_SEMICOLON: printf(" (SEMICOLON), Value: ;\n"); break;
            case TOKEN_COMMENT:   printf(" (COMMENT)  , Value: #...\n"); break; // Value not stored
            case TOKEN_EOF:       printf(" (EOF)\n"); break;
            case TOKEN_ERROR:     printf(" (ERROR)    , Value: \"%s\"\n", current->value ? current->value : "(null)"); break;
            default:              printf(" (UNKNOWN)\n"); break;
        }
        current = current->next;
    }
}