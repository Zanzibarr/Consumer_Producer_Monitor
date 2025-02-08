#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

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
#define MONITOR_TIME 0.1

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

// read
char transaction_log[LOG_SIZE];
size_t log_idx = 0, log_idx_monitor = 0;

// write
int queue_size = 0;
long recieved_messages[N_CONSUMERS], produced_messages = 0;
pthread_mutex_t monitor_mutex;

/* ====================================================== */
/* ======================== UTILS ======================= */
/* ====================================================== */
static void consumer_work() {
    srand(time(NULL));
    int msec = rand() % ((int)((MAX_CONS_TIME - MIN_CONS_TIME) * 1000)) + MIN_CONS_TIME * 1000 + 1;
    usleep(1000 * msec);
}

static void producer_work() { usleep(PROD_TIME * 1000000); }

static void monitor_wait() { usleep(MONITOR_TIME * 1000000); }

static void print_transaction_log() {
    for (size_t t = 0; t < LOG_SIZE; t++) {

        if (log_idx < log_idx_monitor) {
            if (t >= log_idx_monitor || t < log_idx) printf("\033[0;37m%c ", (char)transaction_log[t]);
            else printf("\033[1;30m%c ", (char)transaction_log[t]);
        } else {
            if (t >= log_idx_monitor && t < log_idx) printf("\033[0;37m%c ", (char)transaction_log[t]);
            else printf("\033[1;30m%c ", (char)transaction_log[t]);
        }

    }
    printf("\033[0;37m\n");
}

static void print_monitor() {

    printf("Queue size: %4d - #P: %4ld", queue_size, produced_messages);
    for (size_t t = 0; t < N_CONSUMERS; t++) printf(" - #C%d: %4ld", (int)t, recieved_messages[t]);
    printf("\n");
    
}

void graceful_exit(const int signum) {
    
    pthread_mutex_lock(&queue_mutex);                                                               // block the producer and all consumers

    pthread_mutex_lock(&monitor_mutex);                                                             // give a last look to the monitor
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

    exit(0);

}

/* ====================================================== */
/* ====================== CONSUMER ====================== */
/* ====================================================== */
static void* consumer(void* arg) {

    int item;
    size_t* consumer_name = (size_t*)arg;
    
    while (1) {

        pthread_mutex_lock(&queue_mutex);
        while(read_id == write_id) {
            pthread_cond_wait(&can_digest, &queue_mutex);
        }
        
        item = buffer[read_id];
        read_id = (read_id + 1)%PROD_QUEUE_SIZE;
        num_elem--;

        transaction_log[log_idx] = (int)*consumer_name+'0';
        log_idx = (log_idx + 1) % LOG_SIZE;

        print_transaction_log();

        pthread_cond_signal(&can_produce);
        pthread_mutex_unlock(&queue_mutex);

        consumer_work();

    }

    return NULL;
    
}

/* ====================================================== */
/* ====================== PRODUCER ====================== */
/* ====================================================== */
static void* producer(void* arg) {

    int item = 0;

    while (1) {

        producer_work();

        pthread_mutex_lock(&queue_mutex);
        while((write_id + 1)%PROD_QUEUE_SIZE == read_id) {
            pthread_cond_wait(&can_produce, &queue_mutex);
        }
    
        buffer[write_id] = item;
        write_id = (write_id + 1)%PROD_QUEUE_SIZE;
        num_elem++;

        transaction_log[log_idx] = 'P';
        log_idx = (log_idx + 1) % LOG_SIZE;

        print_transaction_log();

        pthread_cond_signal(&can_digest);
        pthread_mutex_unlock(&queue_mutex);

        item++;

    }

    return NULL;
    
}

/* ====================================================== */
/* ======================= MONITOR ====================== */
/* ====================================================== */
static void* monitor(void* arg) {

    while (1) {

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

        monitor_wait();

    }

    return NULL;

}

/* ====================================================== */
/* ==================== TCP/IP SERVER =================== */
/* ====================================================== */
// TODO

/* ====================================================== */
/* ======================== MAIN ======================== */
/* ====================================================== */

int main(int argc, char* args[]) {

    signal(SIGINT, graceful_exit);

    pthread_mutex_init(&queue_mutex, NULL);
    pthread_cond_init(&can_produce, NULL);
    pthread_cond_init(&can_digest, NULL);
    pthread_mutex_init(&monitor_mutex, NULL);
    
    size_t consumers_names[N_CONSUMERS];
    for (size_t t = 0; t < N_CONSUMERS; t++) consumers_names[t] = t;

    pthread_t threads[N_CONSUMERS + 3];
    pthread_create(&threads[0], NULL, producer, NULL);
    for (size_t t = 0; t < N_CONSUMERS; t++) pthread_create(&threads[t+1], NULL, consumer, &consumers_names[t]);

    pthread_create(&threads[N_CONSUMERS+1], NULL, monitor, NULL);
    // create thread for TCP server

    for(size_t t = 0; t < N_CONSUMERS+3; t++)
        pthread_join(threads[t], NULL);

    return 0;

}