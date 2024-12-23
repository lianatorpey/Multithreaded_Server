/*
 * "PBX" server module.
 * Manages interaction with a client telephone unit (TU).
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <ctype.h>

#include "debug.h"
#include "pbx.h" // includes tu.h already - can't reinclude or linking error when recursive opening
#include "server.h"

/*
 * Thread function for the thread that handles interaction with a client TU.
 * This is called after a network connection has been made via the main server
 * thread and a new thread has been created to handle the connection.
*/
#define CHUNK_SIZE 2048 // Fixed chunk size for reading

void *pbx_client_service(void *arg) {
    int *fd_ptr = (int *)arg; // extract file descriptor passed as an argument
    int fd = *fd_ptr; // take pointer to file descriptor and dereference to access actual value of file descriptor
    free(fd_ptr); // free allocated memory for ptr for rsrc management

    TU *tu = tu_init(fd); // initialize tu for cient using file descriptor
    if (tu == NULL) {
        close(fd);
        return NULL;
    }

    if (pbx_register(pbx, tu, fd) < 0) { // register tu with pbx: associate tu with pbx and assign extension # (use fd for simplicity)
        tu_unref(tu, "Failed registration of TU");
        close(fd);
        return NULL;
    }

    char chunk[CHUNK_SIZE]; // buffer of read chunk
    char *buffer = NULL; // dynamically allocated buffer
    size_t buffer_size = 0; // tracks buffer usage to avoid overwriting
    size_t buffer_used = 0;

    while (1) { // command processing loop (infinite) to continuously read commands from client
        ssize_t bytes_read = read(fd, chunk, sizeof(chunk));
        if (bytes_read <= 0) {
            break;
        }

        // adheres to assignment specifications when there is no limit on length of messages in command
        // Ensure enough space in the dynamic buffer
        if (buffer_size < buffer_used + bytes_read + 1) {
            buffer_size = (buffer_used + bytes_read + 1) * 2;
            buffer = realloc(buffer, buffer_size);
            if (!buffer) {
                perror("Memory allocation failed");
                break;
            }
        }

        // Append new data to the dynamic buffer
        memcpy(buffer + buffer_used, chunk, bytes_read);
        buffer_used += bytes_read;
        buffer[buffer_used] = '\0';

        // Process complete lines
        char *line_start = buffer;
        char *line_end;
        while ((line_end = strstr(line_start, EOL)) != NULL) {
            *line_end = '\0'; // Null-terminate the line
            char *command = line_start;

            // Process the command
            if (strcmp(command, "pickup") == 0) { // TU_PICKUP_CMD
                tu_pickup(tu);
            } else if (strcmp(command, "hangup") == 0) { // TU_HANGUP_CMD
                tu_hangup(tu);
            } else if (strncmp(command, "dial ", 5) == 0) { // TU_DIAL_CMD
                char *ext_str = command + 5;
                while (*ext_str == ' ') ext_str++;
                if (isdigit(*ext_str)) {
                    int ext = atoi(ext_str);
                    pbx_dial(pbx, tu, ext);
                }
            } else if (strncmp(command, "chat ", 5) == 0) { // TU_CHAT_CMD
                char *msg = command + 5;
                tu_chat(tu, msg);
            }

            // Move to the next line
            line_start = line_end + strlen(EOL);
        }

        // Retain any remaining partial line
        buffer_used = strlen(line_start);
        memmove(buffer, line_start, buffer_used);
    }

    free(buffer);
    pbx_unregister(pbx, tu);
    tu_unref(tu, "Client disconnected");
    close(fd);

    return NULL;
}