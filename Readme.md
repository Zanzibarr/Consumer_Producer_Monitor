# Producer / Consumer with remote Monitoring
## Problem:
Producer-(multiple) consumers program with remote status monitoring. The producer and the consumers shall be implemented as threads and the message queue will be hosted in shared memory. Another thread, separate from the producer and the consumers, shall monitor the message queue length, the number of produced messages and the number of received messages for every consumer. A TCP/IP server will allow one or more clients to connect. When a client connects, a new thread is created, handling communication with that client and periodically sending the information collected by the monitor thread.

## Prerequisites
- GCC
- Make

## Installation
- clone this repo
```bash
git clone https://github.com/Zanzibarr/Consumer_Producer_Monitor.git
```
- inside the cloned repo compile the code
```bash
make
```

## Executing the program
- run the server application (producer/consumer, monitor and tcp server)
```bash
./server
```
- run the clients
```bash
./client
```