#include <stdio.h>
#include <string.h>
#include "connection_queue.h"

int connection_queue_init(connection_queue_t *queue) {
    int err = 0;

    // Zero out the struct
    memset(queue, 0, sizeof(connection_queue_t));

    // Initialize the mutex
    if ((err = pthread_mutex_init(&queue->lock, NULL)) != 0) {
        fprintf(stderr, "pthread_mutex_init failed: %s\n", strerror(err));
        return -1;
    }

    // Initialize first condition variable
    if ((err = pthread_cond_init(&queue->full, NULL)) != 0) {
        fprintf(stderr, "pthread_cond_init failed: %s\n", strerror(err));

        // Attempt to free mutex on error
        if ((err = pthread_mutex_destroy(&queue->lock)) != 0) {
            fprintf(stderr,
                    "pthread_mutex_destroy failed: %s\n", strerror(err));
        }

        return -1;
    }

    // Initialize second condition variable, or abort on error
    if ((err = pthread_cond_init(&queue->empty, NULL)) != 0) {
        fprintf(stderr, "pthread_cond_init failed: %s\n", strerror(err));

        // Attempt to free mutex on error
        if ((err = pthread_mutex_destroy(&queue->lock)) != 0) {
            fprintf(stderr,
                    "pthread_mutex_destroy failed: %s\n", strerror(err));
        }

        // Attempt to free previous condition variable on error
        if ((err = pthread_cond_destroy(&queue->full)) != 0) {
            fprintf(stderr, "pthread_cond_destroy failed: %s\n", strerror(err));
        }

        return -1;
    }

    return 0;
}

int connection_enqueue(connection_queue_t *queue, int connection_fd) {
    int err;

    // Obtain lock on the queue
    if ((err = pthread_mutex_lock(&queue->lock)) != 0) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
        return -1;
    }

    // If the queue is already full, release the lock and wait for
    // an available space
    while (queue->length == CAPACITY) {
        if ((err = pthread_cond_wait(&queue->full, &queue->lock)) != 0) {
            fprintf(stderr, "pthread_cond_wait failed: %s\n", strerror(err));

            // Release the lock on failure
            if ((err = pthread_mutex_unlock(&queue->lock)) != 0) {
                fprintf(stderr,
                        "pthread_mutex_unlock failed: %s\n", strerror(err));
            }

            return -1;
        }
    }

    // If the queue is empty and shutdown is indicated, exit
    if (queue->shutdown == 1) {
        // Release the lock
        if ((err = pthread_mutex_unlock(&queue->lock)) != 0) {
            fprintf(stderr,
                    "pthread_mutex_unlock failed: %s\n", strerror(err));
        }
        return -1;
    }

    // Add item to queue
    queue->client_fds[queue->write_idx] = connection_fd;
    queue->write_idx = (queue->write_idx + 1) % CAPACITY;
    queue->length += 1;

    // Signal that the queue is no longer empty
    if ((err = pthread_cond_signal(&queue->empty)) != 0) {
        fprintf(stderr, "pthread_cond_signal failed: %s\n", strerror(err));

        // Release the lock on failure
        if ((err = pthread_mutex_unlock(&queue->lock)) != 0) {
            fprintf(stderr, "pthread_mutex_unlock failed: %s\n", strerror(err));
        }

        return -1;
    }

    // Release the lock
    if ((err = pthread_mutex_unlock(&queue->lock)) != 0) {
        fprintf(stderr, "pthread_mutex_unlock failed: %s\n", strerror(err));
        return -1;
    }

    return 0;
}

int connection_dequeue(connection_queue_t *queue) {
    int err;
    int fd;

    // Obtain lock on the queue
    if ((err = pthread_mutex_lock(&queue->lock)) != 0) {
        fprintf(stderr, "pthread_mutex_lock failed: %s\n", strerror(err));
        return -1;
    }

    // If the queue is already full, release the lock and wait for
    // an available space
    while (queue->length == 0) {
        // If the queue is empty and shutdown is indicated, exit
        if (queue->shutdown == 1) {
            // Release the lock
            if ((err = pthread_mutex_unlock(&queue->lock)) != 0) {
                fprintf(stderr,
                        "pthread_mutex_unlock failed: %s\n", strerror(err));
            }
            return -1;
        }

        // Attempt to wait for a signal
        if ((err = pthread_cond_wait(&queue->empty, &queue->lock)) != 0) {
            fprintf(stderr, "pthread_cond_wait failed: %s\n", strerror(err));

            // Release the lock on failure
            if ((err = pthread_mutex_unlock(&queue->lock)) != 0) {
                fprintf(stderr,
                        "pthread_mutex_unlock failed: %s\n", strerror(err));
            }

            return -1;
        }
    }

    // Remove an item from the queue
    fd = queue->client_fds[queue->read_idx];
    queue->read_idx = (queue->read_idx + 1) % CAPACITY;
    queue->length -= 1;

    // Signal that the queue is no longer empty
    if ((err = pthread_cond_signal(&queue->full)) != 0) {
        fprintf(stderr, "pthread_cond_signal failed: %s\n", strerror(err));

        // Release the lock on failure
        if ((err = pthread_mutex_unlock(&queue->lock)) != 0) {
            fprintf(stderr, "pthread_mutex_unlock failed: %s\n", strerror(err));
        }

        return -1;
    }

    // Release the lock
    if ((err = pthread_mutex_unlock(&queue->lock)) != 0) {
        fprintf(stderr, "pthread_mutex_unlock failed: %s\n", strerror(err));
        return -1;
    }

    return fd;
}

int connection_queue_shutdown(connection_queue_t *queue) {
    int ret = 0;
    int err = 0;
    queue->shutdown = 1;

    if ((err = pthread_cond_broadcast(&queue->full)) != 0) {
        fprintf(stderr, "pthread_cond_broadcast failed: %s\n", strerror(err));
        ret = -1;
    }

    if ((err = pthread_cond_broadcast(&queue->empty)) != 0) {
        fprintf(stderr, "pthread_cond_broadcast failed: %s\n", strerror(err));
        ret = -1;
    }

    return ret;
}

int connection_queue_free(connection_queue_t *queue) {
    int ret = 0;
    int err = 0;
    if ((err = pthread_mutex_destroy(&queue->lock)) != 0) {
        fprintf(stderr, "pthread_mutex_destroy failed: %s\n", strerror(err));
        ret = -1;
    }

    if ((err = pthread_cond_destroy(&queue->full)) != 0) {
        fprintf(stderr, "pthread_cond_destroy failed: %s\n", strerror(err));
        ret = -1;
    }

    if ((err = pthread_cond_destroy(&queue->empty)) != 0) {
        fprintf(stderr, "pthread_cond_destroy failed: %s\n", strerror(err));
        ret = -1;
    }

    return ret;
}
