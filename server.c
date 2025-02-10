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
#define MAX_MONITOR_TIME (LOG_SIZE / (1 / PROD_TIME + N_CONSUMERS / MIN_CONS_TIME))
#define MONITOR_TIME (MAX_MONITOR_TIME / 3)

// TCP server
#define TCP_PORT 9999
#define TCP_MAX_CLIENTS 5
#define TCP_BUFFER_SIZE 500

// I assumed the monitor has no access to the shared memory between producer/consumers (p/c) to consider a more realistic situation
// For an easier implementation, the log that p/c updates for the monitor is stored in the shared memory
// In a realistic scenario, the log is updated through Inter Process Communication insthead of it being in shared memory
// The way the monitor reads the log is the same in both scenarios, the only thing that changes is how the p/c writes on the log (shared memory vs IPC)
// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
// Producer/Consumer
int pcbuf[PROD_QUEUE_SIZE];
size_t pcbuf_read_idx, pcbuf_write_idx;
pthread_cond_t can_produce, can_digest;
pthread_mutex_t queue_mutex;

// Log of transactions for monitor
char transaction_log[LOG_SIZE];
size_t log_write_idx, log_read_idx, pcbuf_size;
// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

// Monitor data
struct monitor_data {
    int updated_switch;
    int queue_size;
    long produced_messages;
    long recieved_messages[N_CONSUMERS];
} monitor_info;

// Coordination between monitor and clients
int active_clients, waiting_clients, active_monitor;
pthread_mutex_t monitor_mutex;
pthread_cond_t monitor_done, clients_done;

// TCP/IP variables
typedef struct {
    int socket;
    int active;
    pthread_t thread;
} ClientInfo;

ClientInfo* clients[TCP_MAX_CLIENTS];
int client_count, tcp_server_socket;

pthread_mutex_t clients_mutex;

// Global flags for graceful shutdown
volatile sig_atomic_t server_running = TRUE;
volatile sig_atomic_t cleanup_in_progress = FALSE;


/* ====================================================== */
/* =================== UTILS FUNCTIONS ================== */
/* ====================================================== */

// consumer works for a random amount of time between MIN_CONS_TIME and MAX_CONS_TIME seconds to consume an item
static inline void consumer_work() { srand(time(NULL)); usleep(1000 * (rand() % ((int)((MAX_CONS_TIME - MIN_CONS_TIME) * 1000)) + MIN_CONS_TIME * 1000 + 1)); }

// consumer works for a fixed amount of time (PROD_TIME seconds) to produce an item
static inline void producer_work() { usleep(PROD_TIME * 1000000); }

// monitor updates every MONITOR_TIME seconds
static inline void monitor_wait() { usleep(MONITOR_TIME * 1000000); }

// view transaction log
static inline void print_transaction_log() {

    for (size_t t = 0; t < LOG_SIZE; t++) {
        if (log_write_idx < log_read_idx) {
            if (t >= log_read_idx || t < log_write_idx)
                printf("\033[0;37m%c ", (char)transaction_log[t]);
            else
                printf("\033[1;30m%c ", (char)transaction_log[t]);
        }
        else {
            if (t >= log_read_idx && t < log_write_idx)
                printf("\033[0;37m%c ", (char)transaction_log[t]);
            else
                printf("\033[1;30m%c ", (char)transaction_log[t]);
        }
    }
    printf("\033[0;37m\n");

}

void update_monitor_status() { monitor_info.updated_switch = monitor_info.updated_switch ? FALSE : TRUE; }

// exits only when the monitor changes its switch variable (wait till new info are in the monitor)
void wait_monitor_update() {
    int monitor_status = monitor_info.updated_switch;
    while (monitor_status == monitor_info.updated_switch) usleep(100000);   // sleep for 0.1 seconds
}

/* ====================================================== */
/* ====================== CONSUMER ====================== */
/* ====================================================== */

// As stated before p/c can communicate to the monitor only through the log or log-related info
static void* consumer(void* arg) {

    size_t* consumer_name = (size_t *)arg;
    printf("Consumer %d active.\n", *((int*)consumer_name));

    int item;

    while (server_running) {

        // mutual exclusion
        pthread_mutex_lock(&queue_mutex);
        while (pcbuf_read_idx == pcbuf_write_idx && server_running) pthread_cond_wait(&can_digest, &queue_mutex);

        // graceful exit
        if (!server_running) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }

        // read item
        item = pcbuf[pcbuf_read_idx];
        pcbuf_read_idx = (pcbuf_read_idx + 1) % PROD_QUEUE_SIZE;
        
        // log transaction
        transaction_log[log_write_idx] = (int)*consumer_name + '0';
        pcbuf_size--;
        log_write_idx = (log_write_idx + 1) % LOG_SIZE;         // I assume the assignment here is atomic (calcualte result in a temporary variable, then assign result to log_write_idx)

        // print_transaction_log();

        // free resources
        pthread_cond_signal(&can_produce);
        pthread_mutex_unlock(&queue_mutex);

        // simulate consume item
        consumer_work();

    }

    // when server is shut down
    printf("Consumer %d shutting down.\n", *((int *)consumer_name));
    return NULL;

}

/* ====================================================== */
/* ====================== PRODUCER ====================== */
/* ====================================================== */

// As stated before p/c can communicate to the monitor only through the log or log-related info
static void* producer(void* arg) {

    printf("Producer active.\n");
    int item = 0;

    while (server_running) {

        // simulate produce time
        producer_work();

        // mutual exclusion
        pthread_mutex_lock(&queue_mutex);
        while ((pcbuf_write_idx + 1) % PROD_QUEUE_SIZE == pcbuf_read_idx && server_running) pthread_cond_wait(&can_produce, &queue_mutex);

        // graceful exit
        if (!server_running) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }

        // write produced item
        pcbuf[pcbuf_write_idx] = item;
        pcbuf_write_idx = (pcbuf_write_idx + 1) % PROD_QUEUE_SIZE;
        
        // log transaction
        transaction_log[log_write_idx] = 'P';
        pcbuf_size++;
        log_write_idx = (log_write_idx + 1) % LOG_SIZE;         // I assume the assignment here is atomic (calcualte result in a temporary variable, then assign result to log_write_idx)

        // print_transaction_log();

        // free resources
        pthread_cond_signal(&can_digest);
        pthread_mutex_unlock(&queue_mutex);

        item++;

    }

    // when server is shut down
    printf("Producer shutting down.\n");
    return NULL;
    
}

/* ====================================================== */
/* ======================= MONITOR ====================== */
/* ====================================================== */

// As stated before, I assumed the monitor can access only the log and other log-related info
static void* monitor(void* arg) {

    printf("Monitor active.\n");

    while (server_running) {

        // block monitor info acces to be sure clients don't get mixed info
        // note that we do not block the producer/consumer dynamics here: new transactions can still be added to the log while reading it
        pthread_mutex_lock(&monitor_mutex);
        while (active_clients > 0) pthread_cond_wait(&clients_done, &monitor_mutex);
        active_monitor = TRUE;
        pthread_mutex_unlock(&monitor_mutex);
    
        // read all new transactions from log
        // log_write_idx tells us, at this moment, which is the last transacion stored in the log
        // 1) log_read_idx != log_write_idx: the monitor didn't read all the info we got at this moment, keep reading
        //      - note that log_write_idx could've been updated during a past iteration, hence we can read that info too (important the order in which we store a new transaction and increase log_write_idx...)
        // 2) log_read_idx == log_write_idx: we've read the last written transaction
        while (log_read_idx != log_write_idx) {
            switch (transaction_log[log_read_idx]) {
                case 'P':
                    monitor_info.produced_messages++;
                    break;
                default:
                    monitor_info.recieved_messages[(size_t)(transaction_log[log_read_idx] - '0')]++;
                    break;
            }
            log_read_idx = (log_read_idx + 1) % LOG_SIZE;
        }
        monitor_info.queue_size = pcbuf_size;

        // tell the clients that the monitor has updated info
        update_monitor_status();
    
        // monitor is updated
        pthread_mutex_lock(&monitor_mutex);
        active_monitor = FALSE;
        if (waiting_clients > 0) pthread_cond_broadcast(&monitor_done);
        pthread_mutex_unlock(&monitor_mutex);

        // wait MONITOR_TIME seconds before updating
        if (server_running) monitor_wait();

    }

    // when server is shut down
    printf("Monitor shutting down.\n");
    return NULL;

}

/* ====================================================== */
/* ==================== TCP/IP SERVER =================== */
/* ====================================================== */

// Function to add a client to our tracking array
static void add_client(ClientInfo* client) {

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
static void remove_client(ClientInfo* client) {

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

static int check_connection(int client_socket) {

    fd_set read_fds;
    struct timeval timeout;
    char tcp_buffer[TCP_BUFFER_SIZE];
    FD_ZERO(&read_fds);
    FD_SET(client_socket, &read_fds);
    
    // Set timeout for select()
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000;   // 1ms timeout

    // wait 1ms to see if client sent something
    int activity = select(client_socket + 1, &read_fds, NULL, NULL, &timeout);

    if (activity < 0) {
        perror("select error in checking client-server connection.");
        return FALSE;
    } else if (activity == 0) {
        // Timeout expired, no data received = no shutdown package recieved
        return TRUE;
    }

    // Check if the client has disconnected
    ssize_t bytes_received = recv(client_socket, tcp_buffer, sizeof(tcp_buffer), 0);
    if (bytes_received == 0) {
        // if client sent 0 bytes then the client must have shut down the connection
        return FALSE;
    } else if (bytes_received < 0) {
        perror("recv error in checking client-server connection.");
        return FALSE;
    }

    // if client sent something we ignore it and return that the connection is still open
    return TRUE;

}

static void* client_handler(void* arg) {

    ClientInfo* client = (ClientInfo*)arg;
    char tcp_buffer[TCP_BUFFER_SIZE];

    while (server_running && client->active) {

        // check if the client has closed the connection
        if (!check_connection(client -> socket)) break;

        // mutual exclusion
        pthread_mutex_lock(&monitor_mutex);
        waiting_clients++;
        while (active_monitor) pthread_cond_wait(&monitor_done, &monitor_mutex);
        waiting_clients--;
        active_clients++;
        pthread_mutex_unlock(&monitor_mutex);

        // read monitor data
        int offset = 27;
        snprintf(tcp_buffer, TCP_BUFFER_SIZE, "Queue size: %4d - #P: %4ld", monitor_info.queue_size, monitor_info.produced_messages);
        for (size_t t = 0; t < N_CONSUMERS; t++) {
            snprintf(tcp_buffer + offset, TCP_BUFFER_SIZE - offset, " - #C%1d: %4ld", (int)t, monitor_info.recieved_messages[t]);
            offset += 12;
        }
        snprintf(tcp_buffer + offset, TCP_BUFFER_SIZE - offset, "\n");

        // free resources
        pthread_mutex_lock(&monitor_mutex);
        active_clients--;
        if (active_clients == 0) pthread_cond_signal(&clients_done);
        pthread_mutex_unlock(&monitor_mutex);

        // send monitor info to client
        ssize_t bytes_sent = send(client->socket, tcp_buffer, strlen(tcp_buffer), 0);
        if (bytes_sent <= 0) {
            client->active = FALSE;
            break;
        }

        // wait for the monitor to have updated info
        wait_monitor_update();

    }

    printf("Client disconnected (socket: %d).\n", client->socket);
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
        perror("socket failed");
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
    if (bind(tcp_server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
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
            if (server_running) perror("Select failed");
            break;
        }
        else if (ready == 0) continue;

        // if server closing: exit
        if (!server_running) break;

        int client_socket = accept(tcp_server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket == -1) {
            if (server_running) perror("Accept failed");
            continue;
        }

        // Check if we can accept more clients
        if (client_count >= TCP_MAX_CLIENTS) {
            printf("Maximum clients reached (%d). Rejecting new connection.\n", TCP_MAX_CLIENTS);
            // Send rejection message to client before closing
            const char* rejection_msg = "CONNECTION_REJECTED:Maximum number of clients reached. Please try again later.\n";
            send(client_socket, rejection_msg, strlen(rejection_msg), 0);
            close(client_socket);
            continue;
        }

        // Create new client info
        ClientInfo *client_info = malloc(sizeof(ClientInfo));
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
        printf("New client connected (socket: %d). Total clients: %d.\n", client_socket, client_count);

    }

    printf("TCP server shutting down.\n");

    return NULL;

}

/* ====================================================== */
/* ================= TERMINATION HANDLER ================ */
/* ====================================================== */

void graceful_exit(const int signum) {

    if (cleanup_in_progress) {
        printf("\nForced exit...\n");
        exit(1);
    }

    cleanup_in_progress = TRUE;
    server_running = FALSE;

    printf("\nInitiating shutdown...\n");

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

    // Close server socket
    if (tcp_server_socket) {
        shutdown(tcp_server_socket, SHUT_RDWR);
        close(tcp_server_socket);
    }

    printf("Waiting for threads to complete...\n");

    // Signal waiting threads
    pthread_cond_broadcast(&can_produce);
    pthread_cond_broadcast(&can_digest);

}

/* ====================================================== */
/* ======================== MAIN ======================== */
/* ====================================================== */

int main(int argc, char *args[]) {

    signal(SIGINT, graceful_exit);

    // initialize shared memory variables
    pcbuf_read_idx = 0;
    pcbuf_write_idx = 0;
    log_read_idx = 0;
    log_write_idx = 0;
    pcbuf_size = 0;
    monitor_info.updated_switch = FALSE;
    monitor_info.produced_messages = 0;
    monitor_info.queue_size = 0;
    for (size_t t = 0; t < N_CONSUMERS; t++) monitor_info.recieved_messages[t] = 0;
    active_clients = 0;
    waiting_clients = 0;
    active_monitor = FALSE;
    client_count = 0;
    
    // initialize mutex
    pthread_mutex_init(&queue_mutex, NULL);
    pthread_mutex_init(&monitor_mutex, NULL);
    pthread_mutex_init(&clients_mutex, NULL);
    pthread_cond_init(&can_produce, NULL);
    pthread_cond_init(&can_digest, NULL);

    // create a thread for each partecipant
    pthread_t producer_thread, consumer_threads[N_CONSUMERS], monitor_thread, tcpserver_thread;
    size_t consumers_names[N_CONSUMERS];

    pthread_create(&producer_thread, NULL, producer, NULL);
    for (size_t t = 0; t < N_CONSUMERS; t++) {
        consumers_names[t] = t;
        pthread_create(&consumer_threads[t], NULL, consumer, &consumers_names[t]);
    }
    pthread_create(&monitor_thread, NULL, monitor, NULL);
    pthread_create(&tcpserver_thread, NULL, tcp_server, NULL);

    // join threads
    pthread_join(producer_thread, NULL);
    for (size_t t = 0; t < N_CONSUMERS; t++)
        pthread_join(consumer_threads[t], NULL);
    pthread_join(monitor_thread, NULL);
    pthread_join(tcpserver_thread, NULL);

    printf("Shutdown complete.\n");

    return 0;

}