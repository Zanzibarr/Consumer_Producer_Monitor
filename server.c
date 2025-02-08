#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <signal.h>

#define TRUE 1
#define FALSE 0

// Producer
#define PROD_QUEUE_SIZE 64
#define PROD_TIME 0.1

// Consumer
#define N_CONSUMERS 10
#define MIN_CONS_TIME 0.5
#define MAX_CONS_TIME 3

// Monitor
#define LOG_SIZE 100
/*
new operations per second <= 1 / prod_time + n_consumers / min_cons_time
time for log to be full >= log_size / n_ops_x_sec = LOG_SIZE / ( 1 / PROD_TIME + N_CONSUMERS / MIN_CONS_TIME )

>> !! MAXIMUM TIME FOR MONITOR REFRESH : LOG_SIZE / ( 1 / PROD_TIME + N_CONSUMERS / MIN_CONS_TIME ) !! <<
*/
#define MAX_MONITOR_TIME LOG_SIZE / ( 1 / PROD_TIME + N_CONSUMERS / MIN_CONS_TIME )
#define MONITOR_TIME MAX_MONITOR_TIME / 3

// TCP server
#define TCP_PORT 9999
#define TCP_MAX_CLIENTS 5
#define TCP_BUFFER_SIZE 500

/* ====================================================== */
/* ============= PRODUCER/CONSUMER DYNAMICS ============= */
/* ====================================================== */
int buffer[PROD_QUEUE_SIZE];
size_t read_id = 0;
size_t write_id = 0;
size_t num_elem = 0;
pthread_mutex_t queue_mutex;
pthread_cond_t can_produce, can_digest;

/* ====================================================== */
/* ================== DATA FOR MONITOR ================== */
/* ====================================================== */
char transaction_log[LOG_SIZE];
size_t log_idx = 0, log_idx_monitor = 0;
int queue_size = 0;
long recieved_messages[N_CONSUMERS], produced_messages = 0;
pthread_mutex_t monitor_mutex;

/* ====================================================== */
/* ================= DATA FOR TCP SERVER ================ */
/* ====================================================== */
typedef struct {
    int socket;
    int active;
    pthread_t thread;
} ClientInfo;

ClientInfo* clients[TCP_MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int client_count = 0;
int tcp_server_socket;

// Global flags for graceful shutdown
volatile sig_atomic_t server_running = TRUE;
volatile sig_atomic_t cleanup_in_progress = FALSE;

/* ====================================================== */
/* ======================== UTILS ======================= */
/* ====================================================== */
static void consumer_work() {
    srand(time(NULL));
    int msec = rand() % ((int)((MAX_CONS_TIME - MIN_CONS_TIME) * 1000)) + MIN_CONS_TIME * 1000 + 1;
    usleep(1000 * msec);
}

static void producer_work() { 
    usleep(PROD_TIME * 1000000); 
}

static void monitor_wait() { 
    usleep(MONITOR_TIME * 1000000); 
}

static int receive(int sd, char *retBuf, int size)
{
    int totSize, currSize;
    totSize = 0;
    while(totSize < size)
    {
        currSize = recv(sd, &retBuf[totSize], size - totSize, 0);
        if(currSize <= 0)
            return -1;
        totSize += currSize;
    }
    return 0;
}

static void print_transaction_log() {
    for (size_t t = 0; t < LOG_SIZE; t++) {
        if (log_idx < log_idx_monitor) {
            if (t >= log_idx_monitor || t < log_idx) 
                printf("\033[0;37m%c ", (char)transaction_log[t]);
            else 
                printf("\033[1;30m%c ", (char)transaction_log[t]);
        } else {
            if (t >= log_idx_monitor && t < log_idx) 
                printf("\033[0;37m%c ", (char)transaction_log[t]);
            else 
                printf("\033[1;30m%c ", (char)transaction_log[t]);
        }
    }
    printf("\033[0;37m\n");
}

static void print_monitor() {
    printf("Queue size: %4d - #P: %4ld", queue_size, produced_messages);
    for (size_t t = 0; t < N_CONSUMERS; t++) 
        printf(" - #C%d: %4ld", (int)t, recieved_messages[t]);
    printf("\n");
}

// Function to add a client to our tracking array
void add_client(ClientInfo* client) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (clients[i] == NULL) {
            clients[i] = client;
            client_count++;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Function to remove a client from our tracking array
void remove_client(ClientInfo* client) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (clients[i] == client) {
            clients[i] = NULL;
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void graceful_exit(const int signum) {
    if (cleanup_in_progress) {
        printf("\nForced exit...\n");
        exit(1);
    }
    
    cleanup_in_progress = TRUE;
    server_running = FALSE;
    
    printf("\nInitiating graceful shutdown...\n");
    
    // Close all client connections
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (clients[i] != NULL) {
            clients[i]->active = FALSE;
            shutdown(clients[i]->socket, SHUT_RDWR);
            close(clients[i]->socket);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    // Update final monitor statistics
    pthread_mutex_lock(&monitor_mutex);
    while (log_idx_monitor != log_idx) {
        switch (transaction_log[log_idx_monitor]) {
            case 'P':
                produced_messages++;
                break;
            default:
                recieved_messages[(size_t)(transaction_log[log_idx_monitor]-'0')]++;
                break;
        }
        log_idx_monitor = (log_idx_monitor + 1) % LOG_SIZE;
    }
    queue_size = num_elem;
    pthread_mutex_unlock(&monitor_mutex);
    
    print_monitor();
    
    // Close server socket
    if (tcp_server_socket) {
        shutdown(tcp_server_socket, SHUT_RDWR);
        close(tcp_server_socket);
    }
    
    printf("Waiting for threads to complete...\n");
    
    // Signal waiting threads
    pthread_cond_broadcast(&can_produce);
    pthread_cond_broadcast(&can_digest);
    
    sleep(2);
    
    printf("Shutdown complete\n");
    exit(0);
}

/* ====================================================== */
/* ====================== CONSUMER ====================== */
/* ====================================================== */
static void* consumer(void* arg) {
    size_t* consumer_name = (size_t*)arg;
    printf("Consumer %d active.\n", *((int*)consumer_name));

    int item;
    
    while(server_running) {
        pthread_mutex_lock(&queue_mutex);
        while(read_id == write_id && server_running) {
            pthread_cond_wait(&can_digest, &queue_mutex);
        }
        
        if (!server_running) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        
        item = buffer[read_id];
        read_id = (read_id + 1)%PROD_QUEUE_SIZE;
        num_elem--;

        transaction_log[log_idx] = (int)*consumer_name+'0';
        log_idx = (log_idx + 1) % LOG_SIZE;

        pthread_cond_signal(&can_produce);
        pthread_mutex_unlock(&queue_mutex);

        consumer_work();
    }
    
    printf("Consumer %d shutting down\n", *((int*)consumer_name));
    return NULL;
}

/* ====================================================== */
/* ====================== PRODUCER ====================== */
/* ====================================================== */
static void* producer(void* arg) {
    printf("Producer active.\n");
    int item = 0;

    while(server_running) {
        producer_work();

        pthread_mutex_lock(&queue_mutex);
        while((write_id + 1)%PROD_QUEUE_SIZE == read_id && server_running) {
            pthread_cond_wait(&can_produce, &queue_mutex);
        }
        
        if (!server_running) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        
        buffer[write_id] = item;
        write_id = (write_id + 1)%PROD_QUEUE_SIZE;
        num_elem++;

        transaction_log[log_idx] = 'P';
        log_idx = (log_idx + 1) % LOG_SIZE;

        pthread_cond_signal(&can_digest);
        pthread_mutex_unlock(&queue_mutex);

        item++;
    }
    
    printf("Producer shutting down\n");
    return NULL;
}

/* ====================================================== */
/* ======================= MONITOR ====================== */
/* ====================================================== */
static void* monitor(void* arg) {
    printf("Monitor active.\n");

    while(server_running) {
        pthread_mutex_lock(&monitor_mutex);
        
        while (log_idx_monitor != log_idx) {
            switch (transaction_log[log_idx_monitor]) {
                case 'P':
                    produced_messages++;
                    break;
                default:
                    recieved_messages[(size_t)(transaction_log[log_idx_monitor]-'0')]++;
                    break;
            }
            log_idx_monitor = (log_idx_monitor + 1) % LOG_SIZE;
        }
        queue_size = num_elem;
        
        pthread_mutex_unlock(&monitor_mutex);
        
        if (server_running) {
            monitor_wait();
        }
    }
    
    printf("Monitor shutting down\n");
    return NULL;
}

/* ====================================================== */
/* ==================== TCP/IP SERVER =================== */
/* ====================================================== */
void* client_handler(void* arg) {
    ClientInfo* client = (ClientInfo*)arg;
    char buffer[TCP_BUFFER_SIZE];

    while (server_running && client->active) {

        //sending to check if connection is closed is a dangerous behaviour
        //if socket connection is closed, server receives terminator like 
        //END_OF_FILE, so recv return 0. Receive is a function to read all input in the buffer
        //man recv for further information
        if(receive(client->socket, buffer, strlen(buffer))) {
            break;
        }

        pthread_mutex_lock(&monitor_mutex);
        int offset = 27;
        snprintf(buffer, TCP_BUFFER_SIZE, "Queue size: %4d - #P: %4ld", queue_size, produced_messages);
        for (size_t t = 0; t < N_CONSUMERS; t++) {
            snprintf(buffer + offset, TCP_BUFFER_SIZE - offset, " - #C%1d: %4ld", (int)t, recieved_messages[t]);
            offset += 12;
        }
        snprintf(buffer + offset, TCP_BUFFER_SIZE - offset, "\n");
        pthread_mutex_unlock(&monitor_mutex);

        ssize_t bytes_sent = send(client->socket, buffer, strlen(buffer), 0);
        if (bytes_sent <= 0) {
            client->active = FALSE;
            break;
        }

        sleep(MONITOR_TIME);
    }

    printf("Client disconnected (socket: %d)\n", client->socket);
    close(client->socket);
    remove_client(client);
    free(client);
    return NULL;
}

static void* tcp_server(void* arg) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Create socket
    if ((tcp_server_socket = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Socket failed");
        exit(1);
    }

    // Set socket options to allow reuse of address
    int opt = 1;
    if (setsockopt(tcp_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(1);
    }

    // Configure server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(TCP_PORT);

    // Bind socket
    if (bind(tcp_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Bind failed");
        close(tcp_server_socket);
        exit(1);
    }

    // Listen for incoming connections
    if (listen(tcp_server_socket, TCP_MAX_CLIENTS) == -1) {
        perror("Listen failed");
        close(tcp_server_socket);
        exit(1);
    }

    printf("Server is listening on port %d...\n", TCP_PORT);

    while (server_running) {
        // Accept with timeout to check server_running flag
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(tcp_server_socket, &read_fds);

        int ready = select(tcp_server_socket + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready < 0) {
            if (server_running) {
                perror("Select failed");
            }
            break;
        } else if (ready == 0) {
            continue;
        }

        if (!server_running) break;

        int client_socket = accept(tcp_server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket == -1) {
            if (server_running) {
                perror("Accept failed");
            }
            continue;
        }

        // Check if we can accept more clients
        if (client_count >= TCP_MAX_CLIENTS) {
            printf("Maximum clients reached. Rejecting new connection.\n");
            close(client_socket);
            continue;
        }

        // Create new client info
        ClientInfo* client_info = malloc(sizeof(ClientInfo));
        if (!client_info) {
            perror("Failed to allocate client info");
            close(client_socket);
            continue;
        }

        client_info->socket = client_socket;
        client_info->active = TRUE;

        // Add to clients array
        add_client(client_info);

        // Create client thread
        if (pthread_create(&client_info->thread, NULL, client_handler, client_info) != 0) {
            perror("Thread creation failed");
            remove_client(client_info);
            close(client_socket);
            free(client_info);
            continue;
        }

        pthread_detach(client_info->thread);
        printf("New client connected (socket: %d). Total clients: %d\n", client_socket, client_count);
    }

    printf("TCP server shutting down\n");
    return NULL;
}

/* ====================================================== */
/* ======================== MAIN ======================== */
/* ====================================================== */

int main(int argc, char* args[]) {
    signal(SIGINT, graceful_exit);

    pthread_t producer_thread, consumer_threads[N_CONSUMERS], monitor_thread, tcpserver_thread;
    size_t consumers_names[N_CONSUMERS];

    // initialize mutex
    pthread_mutex_init(&queue_mutex, NULL);
    pthread_cond_init(&can_produce, NULL);
    pthread_cond_init(&can_digest, NULL);
    pthread_mutex_init(&monitor_mutex, NULL);

    // create a thread for each partecipant
    pthread_create(&producer_thread, NULL, producer, NULL);
    for (size_t t = 0; t < N_CONSUMERS; t++) {
        consumers_names[t] = t;
        pthread_create(&consumer_threads[t], NULL, consumer, &consumers_names[t]);
    }
    pthread_create(&monitor_thread, NULL, monitor, NULL);
    pthread_create(&tcpserver_thread, NULL, tcp_server, NULL);

    // join threads (we never get here)
    pthread_join(producer_thread, NULL);
    for(size_t t = 0; t < N_CONSUMERS; t++) pthread_join(consumer_threads[t], NULL);
    pthread_join(monitor_thread, NULL);
    pthread_join(tcpserver_thread, NULL);

    return 0;

}