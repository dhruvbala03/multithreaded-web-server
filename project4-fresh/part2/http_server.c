#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection_queue.h"
#include "http.h"

#define BUFSIZE 512
#define LISTEN_QUEUE_LEN 5
#define N_THREADS 5

int keep_going = 1;
const char *serve_dir;
connection_queue_t queue;


void handle_sigint(int signo) {
    keep_going = 0;
}


void *thread_func() {

    int err_val = -1;
    void *err_res = (void*)(&err_val);

    while (1) {

        int client_fd = connection_dequeue(&queue);
        if (client_fd == -1) { 
            if (!keep_going) { break; }
            return NULL;
        }

        // read data from client 
        char resource_name[BUFSIZE]; 
        memset(resource_name, 0, BUFSIZE);

        if (read_http_request(client_fd, resource_name) == -1) { 
            if (!keep_going) { break; }
            fprintf(stderr, "Error reading http request\n"); 
            close(client_fd); 
            return err_res; 
        }

        // get resource path from resource name
        int dir_len = strlen(serve_dir);
        char resource_path[dir_len+BUFSIZE];
        memset(resource_path, 0, BUFSIZE);
        strcpy(resource_path, serve_dir); 
        strcat(resource_path, resource_name);

        if (write_http_response(client_fd, resource_path) == -1)
            { fprintf(stderr, "Error writing http response\n"); close(client_fd); return err_res; }

        // cleanup 
        int close_result = close(client_fd);
        if (close_result == -1) { fprintf(stderr, "close failed: %s\n", strerror(close_result)); perror("close"); return err_res; }
    }

    return NULL;
}



int main(int argc, char **argv) {
    // First command is directory to serve, second command is port
    if (argc != 3) {
        printf("Usage: %s <directory> <port>\n", argv[0]);
        return 1;
    }

    serve_dir = argv[1];
    const char *port = argv[2];

    // Catch SIGINT so we can clean up properly
    struct sigaction sigact;
    sigact.sa_handler = handle_sigint;
    sigfillset(&sigact.sa_mask);
    sigact.sa_flags = 0; // Note the lack of SA_RESTART
    if (sigaction(SIGINT, &sigact, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    // Set up hints - we'll take either IPv4 or IPv6, TCP socket type
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // server
    struct addrinfo *server;

    // Set up address info for socket() and connect()
    int ret_val = getaddrinfo(NULL, port, &hints, &server);
    if (ret_val != 0) {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(ret_val));
        return 1;
    }
    // Initialize socket file descriptor
    int sock_fd = socket(server->ai_family, server->ai_socktype, server->ai_protocol);
    if (sock_fd == -1) {
        perror("socket");
        freeaddrinfo(server);
        return 1;
    }
    // Bind socket to receive at a specific port
    if (bind(sock_fd, server->ai_addr, server->ai_addrlen) == -1) {
        perror("bind");
        freeaddrinfo(server);
        close(sock_fd);
        return 1;
    }
    freeaddrinfo(server);
    // Designate socket as a server socket
    if (listen(sock_fd, LISTEN_QUEUE_LEN) == -1) {
        perror("listen");
        close(sock_fd);
        return 1;
    }

    // Set up connection queue
    connection_queue_init(&queue);

    // block all signals in worker threads
    sigset_t main_sigset, worker_sigset;
    if (sigfillset(&worker_sigset) == -1) { perror("sigfillset"); close(sock_fd); return 1; }
    if (sigprocmask(SIG_SETMASK, &worker_sigset, &main_sigset) == -1) { perror("sigprocmask"); close(sock_fd); return 1; }

    // Set up worker threads
    pthread_t threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        int create_result = pthread_create(&threads[i], NULL, thread_func, NULL);
        if (create_result == -1) { 
            fprintf(stderr, "pthread_create failed: %s\n", strerror(create_result)); 
            if (close(sock_fd) == -1) { perror("close"); }
            connection_queue_free(&queue);
            return 1;
        }
    }

    // restore old signal mask in main thread
    if (sigprocmask(SIG_SETMASK, &main_sigset, NULL) == -1) {
        perror("sigprocmask");
        if (close(sock_fd) == -1) { perror("close"); }
        return 1;
    }

    // Main loop
    ret_val = 0;
    while (keep_going != 0) {
        // wait to receive a connection request from client
        // don't bother saving client address information
        int client_fd = accept(sock_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno != EINTR) { 
                fprintf(stderr, "accept failed: %s\n", strerror(errno)); 
                if (close(sock_fd) == -1) { perror("close"); }
                ret_val = 1; 
            } 
            break;
        }

        if (connection_enqueue(&queue, client_fd) == -1) { 
            fprintf(stderr, "Failed to enqueue connection\n");
            if (close(sock_fd) == -1) { perror("close"); }
            connection_queue_free(&queue); 
            ret_val = 1; 
        }
    }

    // (close sock_fd on error)
    if (ret_val != 0) {
        if (connection_queue_free(&queue) == -1) {
            fprintf(stderr, "Failed to free queue\n");
        }
        if (close(sock_fd) == -1) { perror("close"); }
        return 1;
    }

    // Shutdown the queue
    int shutdown_result = connection_queue_shutdown(&queue);
    if (shutdown_result == -1) {
        fprintf(stderr, "Failed to shutdown connection queue\n");
        if (connection_queue_free(&queue) == -1) {
            fprintf(stderr, "Failed to free queue\n");
        }
        if (close(sock_fd) == -1) { perror("close"); }
        return 1;
    }

    // join worker threads
    for (int i = 0; i < N_THREADS; i++) {
        int join_result = pthread_join(threads[i], NULL);
        if (join_result != 0) { fprintf(stderr, "pthread_join failed: %s\n", strerror(errno)); ret_val = 1; }
    }

    // Free the queue
    if (connection_queue_free(&queue) == -1) {
        fprintf(stderr, "Failed to free connection queue\n");
        if (close(sock_fd) == -1) { perror("close"); }
        return 1;
    }

    // remaining cleanup
    if (close(sock_fd) == -1) { perror("close"); return 1; }

    return ret_val;
}
