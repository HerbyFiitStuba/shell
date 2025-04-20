; ===========================================================================================
; ##################################### SHELL README ########################################
; ===========================================================================================
; Author:          [Matej Herzog]
; Task:            [Implement Basic Shell]
;                  [Bonus tasks: 3, 4, 7, 12, 13, 14, 17, 18, 21]
; Submission Date: [20.4.2025]
; Year:            [2025], Academic Year: [2], Semester: [2], Department: [FIIT B-INFO 3]

This document describes how to compile and run my shell program.

; ===========================================================================================
; #################################### COMPILATION ##########################################
; ===========================================================================================

To compile the program, navigate to the project's root directory (where the Makefile is located) in your terminal and run:

```bash
make
```

or 

```bash
make all
```

This will compile all the source files located in the `src/` directory and create an executable file named `shell` in the root directory.

To clean up the compiled object files and the executable, run:

```bash
make clean
```

; ===========================================================================================
; ######################################### USAGE ###########################################
; ===========================================================================================

The program can run in two modes: Server or Client.

**Server Mode (Default):**

Listens for incoming client connections and provides a shell interface to each connected client.

```bash
./shell [-s] -p <port> [-i <bind_addr>] [-v] [-l <logfile>] [-t <timeout>] [-d] [-h]
```

*   `-s`: Explicitly select server mode (this is the default if neither -s nor -c is given).
*   `-p <port>`: **Required.** The TCP port number the server will listen on (e.g., 8080).
*   `-i <bind_addr>`: Optional. The IP address the server should bind to. If omitted, it binds to all available interfaces (0.0.0.0).
*   `-v`: Optional. Enable verbose logging to stderr (or the log file if -l is used). Shows DEBUG messages.
*   `-l <logfile>`: Optional. Redirect server logs (INFO, ERROR, DEBUG if -v) to the specified file instead of stderr.
*   `-t <timeout>`: Optional. Set the client inactivity timeout in seconds. Clients idle for this duration will be disconnected. Defaults to 600 seconds (10 minutes). A value of 0 disables the timeout.
*   `-d`: Optional. Run the server as a daemon process in the background.
*   `-h`: Display a help message and exit.

**Client Mode:**

Connects to a running shell server.

```bash
./shell -c -i <server_addr> -p <port> [-v] [-l <logfile>] [-h]
```

*   `-c`: **Required.** Select client mode.
*   `-i <server_addr>`: **Optional** The IP address of the server to connect to (e.g., 127.0.0.1), defaults to 127.0.0.1.
*   `-p <port>`: **Required.** The TCP port number the server is listening on.
*   `-v`: Optional. Enable verbose logging for the client process.
*   `-l <logfile>`: Optional. Redirect client logs to the specified file instead of stderr.
*   `-h`: Display a help message and exit.

**Shell Interaction (Client Side):**

Once connected, the client will receive a prompt (e.g., `HH:MM user@host# `). You can then type commands.

*   **Commands:** Standard Linux commands (e.g., `ls`, `echo`, `pwd`).
*   **Sequential Execution:** Separate commands with a semicolon (`;`). They will run one after the other. (e.g., `pwd ; ls -l`)
*   **Pipelines:** Pipe the standard output of one command to the standard input of the next using the pipe symbol (`|`). (e.g., `ls -l | grep .c | wc -l`)
*   **Input Redirection:** Redirect standard input from a file using `<`. (e.g., `wc -l < somefile.txt`)
*   **Output Redirection:** Redirect standard output to a file using `>`. This will overwrite the file if it exists or create it if it doesn't. (e.g., `ls > filelist.txt`)
*   **Comments:** Lines starting with `#` are ignored.
*   **Built-in Commands:**
    *   `help`: Display help information about usage and built-ins.
    *   `quit`: Disconnect the current client from the server.
    *   `halt`: Request the server to shut down gracefully.
    *   `stat`: Display server status, including listening address/port and connected client information.
    *   `abort <n>`: Request the server to disconnect the client specified by index `<n>` (0-based index shown in `stat`).

; ===========================================================================================
; ################################# IMPLEMENTED FEATURES ####################################
; ===========================================================================================

*   **Client/Server Architecture:** Uses TCP sockets for communication.
*   **Non-blocking I/O:** Uses `epoll` with non-blocking sockets on the server for efficient handling of multiple clients.
*   **Command Execution:** Executes external commands using `fork` and `execvp`.
*   **Pipelines (`|`):** Supports chaining multiple commands via pipes. Standard output/error of commands in a pipeline (except the last) are redirected appropriately.
*   **I/O Redirection (`<`, `>`):** Supports redirecting standard input from a file and standard output/error to a file for individual commands.
*   **Sequential Execution (`;`):** Allows multiple commands or pipelines to be run in sequence.
*   **Built-in Commands:** Implements `help`, `quit`, `halt`, `stat`, and `abort` directly within the shell process on the server side.
*   **Client Timeout:** Server disconnects clients that are inactive for a configurable period (`-t` option).
*   **Logging:** Provides logging for server and client operations (`-l`, `-v` options). Logs include timestamps and process IDs.
*   **Daemonization:** Server can run as a background daemon process (`-d` option).
*   **Dynamic Client Management:** Server dynamically allocates and manages state for connected clients.
*   **Basic Error Handling:** Includes checks for invalid command syntax, file opening errors, network errors, etc.
*   **Prompt Generation:** Server sends a dynamic prompt (`HH:MM user@host# `) to the client.
*   **Support for Quotes:** Tokenizer and Parser support `"` or `'` for grouping arguments containing spaces.