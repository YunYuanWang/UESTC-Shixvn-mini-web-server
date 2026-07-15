#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>

/*
 * thread_pool.h — fixed-size thread pool with a blocking work queue
 *
 * Workers block on a condition variable when the queue is empty.
 * The main thread enqueues client_fd tasks; workers dequeue and
 * process them through the existing HTTP request handler.
 *
 * Shutdown protocol:
 *   1. thread_pool_shutdown() sets the shutdown flag and broadcasts
 *      the condition variable to wake all blocked workers.
 *   2. Workers finish any in-flight task, then exit.
 *   3. thread_pool_destroy() joins all workers and frees resources.
 */

/* ---- a single unit of work ---- */
typedef struct {
    int client_fd;   /* connected socket fd to handle */
} task_t;

/* ---- thread pool ---- */
typedef struct {
    task_t         *tasks;        /* circular buffer */
    int             capacity;     /* max queued tasks */
    int             count;        /* current number of queued tasks */
    int             head;         /* dequeue position */
    int             tail;         /* enqueue position */

    pthread_mutex_t mutex;        /* protects the queue + shutdown flag */
    pthread_cond_t  not_empty;    /* signaled when queue becomes non-empty */
    pthread_cond_t  not_full;     /* signaled when queue has room */

    pthread_t      *workers;      /* worker thread handles */
    int             num_workers;  /* number of worker threads */

    int             shutdown;     /* 1 = draining, workers should exit */

    /* statistics (protected by stats_mutex) */
    int             total_processed;
    int             total_errors;
    pthread_mutex_t stats_mutex;
} thread_pool_t;

/*
 * Create a thread pool.
 *
 *   num_workers     — number of worker threads (e.g. 4)
 *   queue_capacity  — max pending tasks in the queue
 *
 * Workers are started immediately and block on the empty queue.
 * Returns a heap-allocated pool on success, NULL on failure.
 */
thread_pool_t *thread_pool_create(int num_workers, int queue_capacity);

/*
 * Enqueue a client fd for processing.
 * Blocks (on not_full CV) if the queue is at capacity.
 * Returns  0 on success,
 *         -1 if the pool is already shutting down.
 */
int thread_pool_add_task(thread_pool_t *pool, int client_fd);

/*
 * Signal the pool to drain and stop.
 * Wakes all blocked workers so they can exit.
 * No new tasks will be accepted after this call.
 */
void thread_pool_shutdown(thread_pool_t *pool);

/*
 * Wait for all workers to finish and release all resources.
 * Must be called after thread_pool_shutdown().
 * If out_processed / out_errors are non-NULL, the final stats are
 * written there before the pool is freed.
 */
void thread_pool_destroy(thread_pool_t *pool,
                         int *out_processed, int *out_errors);

/*
 * Return the number of successfully processed tasks.
 */
int thread_pool_get_processed(thread_pool_t *pool);

/*
 * Return the number of tasks that resulted in an error.
 */
int thread_pool_get_errors(thread_pool_t *pool);

#endif /* THREAD_POOL_H */
