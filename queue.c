#include "queue.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static inline size_t next(struct queue *q, size_t cur) {
    return (cur + q->item_size) % q->size;
}

static inline void set_timeout(struct timespec *ts, unsigned int timeout_ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000;
    ts->tv_nsec += (timeout_ms % 1000) * 1000000;
}

bool queue_init(struct queue *q, size_t item_size, size_t max_items) {
    size_t size = max_items * item_size;
    q->data = malloc(size);
    if (q->data == NULL) {
        return false;
    }

    q->head = 0;
    q->tail = 0;
    q->item_size = item_size;
    q->size = size;
    q->terminated = false;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return true;
}

bool queue_push_noblock(struct queue *q, void *v) {
    pthread_mutex_lock(&q->mutex);

    if (next(q, q->head) == q->tail) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    memcpy(&q->data[q->head], v, q->item_size);
    q->head = next(q, q->head);

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);

    return true;
}

bool queue_push(struct queue *q, void *v, unsigned int timeout_ms) {
    pthread_mutex_lock(&q->mutex);

    while (next(q, q->head) == q->tail) {
        int ret = 0;
        if (timeout_ms > 0) {
            struct timespec ts;
            set_timeout(&ts, timeout_ms);
            ret = pthread_cond_timedwait(&q->not_empty, &q->mutex, &ts);
        } else {
            pthread_cond_wait(&q->not_empty, &q->mutex);
        }

        if (q->terminated || ret != 0) {
            pthread_mutex_unlock(&q->mutex);
            return false;
        }
    }

    memcpy(&q->data[q->head], v, q->item_size);
    q->head = next(q, q->head);

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);

    return true;
}

bool queue_pop_noblock(struct queue *q, void *v) {
    pthread_mutex_lock(&q->mutex);

    if (q->tail == q->head) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    memcpy(v, &q->data[q->tail], q->item_size);
    q->tail = next(q, q->tail);

    pthread_mutex_unlock(&q->mutex);

    return true;
}

bool queue_pop(struct queue *q, void *v, unsigned int timeout_ms) {
    pthread_mutex_lock(&q->mutex);

    while (q->tail == q->head) {
        int ret = 0;
        if (timeout_ms > 0) {
            struct timespec ts;
            set_timeout(&ts, timeout_ms);
            ret = pthread_cond_timedwait(&q->not_empty, &q->mutex, &ts);
        } else {
            pthread_cond_wait(&q->not_empty, &q->mutex);
        }

        if (q->terminated || ret != 0) {
            pthread_mutex_unlock(&q->mutex);
            return false;
        }
    }

    memcpy(v, &q->data[q->tail], q->item_size);
    q->tail = next(q, q->tail);

    pthread_mutex_unlock(&q->mutex);

    return true;
}

bool queue_full(struct queue *q) {
    pthread_mutex_lock(&q->mutex);
    bool full = next(q, q->head) == q->tail;
    pthread_mutex_unlock(&q->mutex);
    return full;
}

bool queue_empty(struct queue *q) {
    pthread_mutex_lock(&q->mutex);
    bool empty = q->tail == q->head;
    pthread_mutex_unlock(&q->mutex);
    return empty;
}

void queue_terminate(struct queue *q) {
    pthread_mutex_lock(&q->mutex);
    q->terminated = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

void queue_deinit(struct queue *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    free(q->data);
}
