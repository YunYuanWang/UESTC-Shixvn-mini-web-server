#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>

/*
 * thread_pool.h — dynamic thread pool with core/max sizing
 *
 * The pool starts with core_size workers.  When all workers are busy
 * and the queue is non-empty, the pool automatically scales up by
 * spawning additional workers (up to max_size).
 *
 * Extra workers (those beyond core_size) use a timed wait when the
 * queue is empty: if no task arrives within IDLE_TIMEOUT_MS, they
 * exit, shrinking the pool back toward core_size.  Core workers wait
 * indefinitely.
 *
 * Shutdown protocol:
 *   1. thread_pool_shutdown() sets the shutdown flag and broadcasts
 *      the condition variable to wake all blocked workers.
 *   2. Workers finish any in-flight task, then exit.
 *   3. thread_pool_destroy() joins all remaining workers and frees
 *      resources.
 */

#define IDLE_TIMEOUT_MS  1000   /* extra worker idle timeout before exit */
#define MAX_POOL_WORKERS 64     /* hard cap on pool->workers[] array */

/* ---- a single unit of work ---- */
typedef struct {
    int client_fd;   /* connected socket fd to handle */
} task_t;

/* ---- per-worker info passed as thread argument ---- */
typedef struct {
    struct thread_pool *pool;   /* back-pointer to the pool */
    int               slot;     /* index in pool->workers[] */
    int               is_core;  /* 1 = core worker, 0 = extra (scalable) */
} worker_arg_t;

/* ---- thread pool ---- */
typedef struct thread_pool {
    task_t         *tasks;          /* circular buffer */
    int             capacity;       /* max queued tasks */
    int             count;          /* current number of queued tasks */
    int             head;           /* dequeue position */
    int             tail;           /* enqueue position */

    pthread_mutex_t mutex;          /* protects queue + worker metadata */
    pthread_cond_t  not_empty;      /* signaled when queue becomes non-empty */
    pthread_cond_t  not_full;       /* signaled when queue has room */

    pthread_t       workers[MAX_POOL_WORKERS]; /* thread handles (fixed) */
    int             worker_active[MAX_POOL_WORKERS]; /* 1 = slot in use */
    int             num_workers;    /* current active worker count */
    int             core_size;      /* minimum workers (never scaled down) */
    int             max_size;       /* maximum workers (scale-up cap) */

    int             shutdown;       /* 1 = draining, all workers exit */

    /* statistics (protected by stats_mutex) */
    int             total_processed;
    int             total_errors;
    int             peak_workers;   /* highest num_workers reached */
    pthread_mutex_t stats_mutex;
} thread_pool_t;

/*
 * Create a thread pool.
 *
 *   core_size       — initial and minimum number of workers
 *   max_size        — maximum number of workers (scale-up cap)
 *   queue_capacity  — max pending tasks in the queue
 *
 * Returns a heap-allocated pool on success, NULL on failure.
 */
thread_pool_t *thread_pool_create(int core_size, int max_size,
                                  int queue_capacity);

/*
 * Enqueue a client fd for processing.
 * Blocks (on not_full CV) if the queue is at capacity.
 * May trigger scale-up if all workers are busy.
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

/*
 * Return the peak number of concurrent workers.
 */
int thread_pool_get_peak_workers(thread_pool_t *pool);

#endif /* THREAD_POOL_H */
