#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>

#define SERVER_IP "127.0.0.1"
#define PORT 9999
#define BUFFER_SIZE 500

#define FALSE 0
#define TRUE 1

int client_socket;
volatile sig_atomic_t close_connection = FALSE;

void graceful_exit(int signum) {
    close_connection = TRUE;
    // Send shutdown signal through socket to ensure clean server-side disconnect
    shutdown(client_socket, SHUT_RDWR);
}

int main() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = graceful_exit;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    
    // Register signal handlers
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Failed to set SIGINT handler");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("Failed to set SIGTERM handler");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

    // Create socket
    if ((client_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    // Configure server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(client_socket);
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Connection failed");
        close(client_socket);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server. Receiving data...\n");
    printf("Press Ctrl+C to exit gracefully\n");

    FILE* file = fopen("client_data.txt", "a");
    if (!file) {
        perror("File open failed");
        close(client_socket);
        exit(EXIT_FAILURE);
    }

    // Receive data loop
    while (!close_connection) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes_received < 0 && !close_connection) {
            perror("Receive failed");
            break;
        } else if (bytes_received == 0 || close_connection) {
            printf("\nConnection closed gracefully\n");
            break;
        }

        // Write to file
        fprintf(file, "%s", buffer);
        fflush(file);  // Ensure data is written immediately
    }

    printf("Cleaning up and exiting...\n");
    fclose(file);
    close(client_socket);
    
    return 0;
}