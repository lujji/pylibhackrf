#ifndef QUEUE_H
#define QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/**
 * Thread-safe queue
 */
struct queue {
    char *data;
    size_t item_size;
    size_t size;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    volatile bool terminated;
    volatile size_t head;
    volatile size_t tail;
};

/**
 * Initialize a queue
 *
 * @param q queue
 * @param item_size item size in bytes
 * @param max_items maximum number of items in the queue
 * @return false if memory allocation failed
 */
bool queue_init(struct queue *q, size_t item_size, size_t max_items);

/**
 * Destroy the queue and allocated resources. Call only after all threads have
 * stopped using the queue
 *
 * @param q queue
 */
void queue_deinit(struct queue *q);

/**
 * Terminate the queue. This will cause all blocking operations to return
 *
 * @param q queue
 */
void queue_terminate(struct queue *q);

/**
 * Check if the queue is full
 *
 * @param q queue
 */
bool queue_full(struct queue *q);

/**
 * Check if the queue is empty
 *
 * @param q queue
 */
bool queue_empty(struct queue *q);

/**
 * Push an item to the queue. This function will block if the queue is full
 *
 * @param q queue
 * @param v item to push
 * @param timeout_ms timeout in milliseconds, 0 will block forever
 *
 * @return true on success
 */
bool queue_push(struct queue *q, void *v, unsigned int timeout_ms);

/**
 * Push an item to the queue (non-blocking).
 *
 * @param q queue
 * @param v item to push
 * @return false if the queue is full
 */
bool queue_push_noblock(struct queue *q, void *v);

/**
 * Pop an item from the queue. This function will block if the queue is empty
 *
 * @param q queue
 * @param v pointer to store the item
 * @param timeout_ms timeout in milliseconds, 0 will block forever
 *
 * @return true if valid item was popped
 */
bool queue_pop(struct queue *q, void *v, unsigned int timeout_ms);

/**
 * Pop an item from the queue (non-blocking)
 *
 * @param q queue
 * @param v pointer to store the item
 * @return false if the queue is empty
 */
bool queue_pop_noblock(struct queue *q, void *v);

#endif // QUEUE_H
