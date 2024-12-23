#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "pbx.h" // already includes tu.h (not needed in this file)
#include "server.h"
#include "debug.h"

// Forward declarations
static void terminate_server(int status);
static void sighup_handler(int sig);

// File descriptor for the server's listening socket
static int server_socket;

static atomic_bool shutdown_request = false; // atomic flag for sig handler

/*
 * "PBX" telephone exchange simulation.
 *
 * Usage: pbx -p <port>
 */
int main(int argc, char *argv[]) {
    int port = 8080; // Default port number
    int opt;

    // Parse command-line options to extract the port number
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p': // Port option
                port = atoi(optarg);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "ERROR: Invalid port number\n");
                    exit(EXIT_FAILURE);
                }
                break;
            default:
                fprintf(stderr, "ERROR Usage: %s -p <port>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Initialize the PBX module - for ther server
    pbx = pbx_init();

    // Install a SIGHUP handler for server shutdown
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighup_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGHUP, &sa, NULL) == -1) {
        fprintf(stderr, "ERROR: failed installing a SIGHUP handler with sigaction\n");
        terminate_server(EXIT_FAILURE);
    }

    // Create socket for server
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        fprintf(stderr, "ERROR: failed creating a socket for the server (if reusing socket, possible time_wait violation)\n");
        terminate_server(EXIT_FAILURE);
    }

    // Configure server address structure
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Bind the server socket to the specified port
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        fprintf(stderr, "ERROR: failed binding server socket to port number specified (if reusing port #, possible time_wait violation)\n");
        terminate_server(EXIT_FAILURE);
    }

    // Start listening for incoming connections
    if (listen(server_socket, SOMAXCONN) == -1) {
        fprintf(stderr, "ERROR: failed to listen for incoming connections\n");
        terminate_server(EXIT_FAILURE);
    }

    fprintf(stderr, "Server listening on port %d...\n", port);

    while (1) { // Main server loop: Accept and handle incoming client connections
        if (atomic_load(&shutdown_request)) {
            terminate_server(EXIT_SUCCESS);
            break;
        }

        int client_socket = accept(server_socket, NULL, NULL);
        if (client_socket == -1) {
            if (errno == EINTR) {
                break; // Interrupt by SIGHUP
            }
            fprintf(stderr, "ERROR: failed to accept and handle incoming new client connection\n");
            continue;
        }

        int *client_fd = malloc(sizeof(int)); // need memory so pass file descriptor to thread
        if (client_fd == NULL) {
            fprintf(stderr, "ERROR: malloc failed\n");
            close(client_socket);
            continue;
        }
        *client_fd = client_socket;

        pthread_t thread; //  new thread for each client connection
        if (pthread_create(&thread, NULL, pbx_client_service, client_fd) != 0) {
            fprintf(stderr, "ERROR: failed to create new thread for client connection\n");
            free(client_fd);
            close(client_socket);
            continue;
        }
        if (pthread_detach(thread) != 0) {
            fprintf(stderr, "ERROR: failed to detach thread so on termination resources are NOT clean automatically - must join thread manually\n");
        }
    }

    return 0;
}

/*
 * SIGHUP handler for clean server shutdown.
 */
static void sighup_handler(int sig) {
    // terminate_server(EXIT_SUCCESS); // idk if function async safe
    atomic_store(&shutdown_request, true);
}

/*
 * Cleanly shut down the server.
 */
static void terminate_server(int status) {
    fprintf(stderr, "Shutting down PBX...\n");

    // Close server socket
    if (server_socket >= 0) {
        close(server_socket);
        fprintf(stderr, "Server socket closed\n");
    }

    // Shut down the PBX module
    pbx_shutdown(pbx);

    exit(status);
}