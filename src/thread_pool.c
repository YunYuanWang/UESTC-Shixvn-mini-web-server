/*
 * thread_pool.c — fixed-size thread pool with a blocking work queue
 *
 * Architecture:
 *   - Circular buffer of task_t (each wrapping a client_fd).
 *   - Mutex + two condition variables (not_empty / not_full) guard the queue.
 *   - Worker threads loop: wait for task → dequeue → process → close fd.
 *   - Shutdown: set flag, broadcast CV, join all workers.
 *
 * Synchronization (POSIX style, following os_course conventions):
 *   - pthread_mutex_t  mutex       protects queue + shutdown
 *   - pthread_cond_t   not_empty   workers wait here when queue is empty
 *   - pthread_cond_t   not_full    enqueuer waits here when queue is full
 *   - pthread_mutex_t  stats_mutex protects total_processed / total_errors
 */

#include "../include/thread_pool.h"
#include "../include/log.h"
#include "../include/request_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ================================================================
 *  worker thread function
 * ================================================================ */
static void *worker_thread_func(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    char msg[128];
    int worker_id;

    /*
     * Each worker gets a unique id (1-based) stored in a thread-local
     * variable inside request_handler so that log messages include the
     * worker number.
     */
    {
        static int next_id = 0;
        static pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&id_mutex);
        next_id++;
        worker_id = next_id;
        pthread_mutex_unlock(&id_mutex);
    }

    /* set the worker label for request_handler log messages */
    request_handler_set_worker_label(worker_id);

    snprintf(msg, sizeof(msg), "[Worker-%d] started (TID %lu)",
             worker_id, (unsigned long)pthread_self());
    log_info(msg);

    while (1) {
        int client_fd;

        /* ---- wait for a task (or shutdown) ---- */
        pthread_mutex_lock(&pool->mutex);

        while (pool->count == 0 && !pool->shutdown) {
            /* queue empty — block until a task arrives or shutdown */
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        }

        /* exit condition: shutdown AND queue drained */
        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* dequeue one task */
        client_fd = pool->tasks[pool->head].client_fd;
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;

        /* signal the enqueuer that there is now room */
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->mutex);

        /* ---- process the HTTP request ---- */
        {
            int ret;

            snprintf(msg, sizeof(msg),
                     "[Worker-%d] processing client_fd=%d",
                     worker_id, client_fd);
            log_info(msg);

            ret = request_handler_handle_connection(client_fd);

            /* worker is responsible for closing the client fd */
            close(client_fd);

            snprintf(msg, sizeof(msg),
                     "[Worker-%d] connection closed (fd=%d)",
                     worker_id, client_fd);
            log_info(msg);

            /* update stats */
            pthread_mutex_lock(&pool->stats_mutex);
            pool->total_processed++;
            if (ret != 0) {
                pool->total_errors++;
            }
            pthread_mutex_unlock(&pool->stats_mutex);
        }
    }

    snprintf(msg, sizeof(msg), "[Worker-%d] exiting", worker_id);
    log_info(msg);

    return NULL;
}

/* ================================================================
 *  public API
 * ================================================================ */

thread_pool_t *thread_pool_create(int num_workers, int queue_capacity) {
    thread_pool_t *pool;
    int i;

    if (num_workers < 1 || queue_capacity < 1) {
        return NULL;
    }

    pool = (thread_pool_t *)calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }

    pool->tasks = (task_t *)calloc((size_t)queue_capacity, sizeof(task_t));
    if (pool->tasks == NULL) {
        free(pool);
        return NULL;
    }

    pool->workers = (pthread_t *)calloc((size_t)num_workers,
                                        sizeof(pthread_t));
    if (pool->workers == NULL) {
        free(pool->tasks);
        free(pool);
        return NULL;
    }

    pool->capacity    = queue_capacity;
    pool->count       = 0;
    pool->head        = 0;
    pool->tail        = 0;
    pool->num_workers = num_workers;
    pool->shutdown    = 0;

    /* init sync primitives */
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);
    pthread_mutex_init(&pool->stats_mutex, NULL);

    /* spawn worker threads */
    for (i = 0; i < num_workers; i++) {
        if (pthread_create(&pool->workers[i], NULL,
                           worker_thread_func, pool) != 0) {
            /* on failure, shut down already-created workers */
            log_error("[ThreadPool] pthread_create failed");
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->not_empty);

            /* join the ones we did create */
            {
                int j;
                for (j = 0; j < i; j++) {
                    pthread_join(pool->workers[j], NULL);
                }
            }

            /* tear down */
            pthread_mutex_destroy(&pool->stats_mutex);
            pthread_cond_destroy(&pool->not_full);
            pthread_cond_destroy(&pool->not_empty);
            pthread_mutex_destroy(&pool->mutex);
            free(pool->workers);
            free(pool->tasks);
            free(pool);
            return NULL;
        }
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[ThreadPool] created — %d workers, queue capacity %d",
                 num_workers, queue_capacity);
        log_info(msg);
    }

    return pool;
}

int thread_pool_add_task(thread_pool_t *pool, int client_fd) {
    if (pool == NULL) {
        return -1;
    }

    pthread_mutex_lock(&pool->mutex);

    /* reject new tasks once we're shutting down */
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    /* block while the queue is full */
    while (pool->count >= pool->capacity && !pool->shutdown) {
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    }

    /* re-check shutdown (may have been woken by shutdown broadcast) */
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    /* enqueue */
    pool->tasks[pool->tail].client_fd = client_fd;
    pool->tail = (pool->tail + 1) % pool->capacity;
    pool->count++;

    /* wake one waiting worker */
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}

void thread_pool_shutdown(thread_pool_t *pool) {
    if (pool == NULL) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    /* wake all blocked workers so they can see shutdown == 1 and exit */
    pthread_cond_broadcast(&pool->not_empty);
    /* wake anyone blocked in add_task waiting for room */
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->mutex);

    log_info("[ThreadPool] shutdown signaled");
}

void thread_pool_destroy(thread_pool_t *pool,
                         int *out_processed, int *out_errors) {
    int i;

    if (pool == NULL) {
        return;
    }

    log_info("[ThreadPool] waiting for workers to exit...");

    for (i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->workers[i], NULL);
        {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "[ThreadPool] worker thread %d joined", i + 1);
            log_info(msg);
        }
    }

    log_info("[ThreadPool] all workers finished");

    /* snapshot final stats before freeing */
    if (out_processed != NULL) {
        *out_processed = pool->total_processed;
    }
    if (out_errors != NULL) {
        *out_errors = pool->total_errors;
    }

    /* tear down sync primitives */
    pthread_mutex_destroy(&pool->stats_mutex);
    pthread_cond_destroy(&pool->not_full);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->mutex);

    free(pool->workers);
    free(pool->tasks);
    free(pool);
}

int thread_pool_get_processed(thread_pool_t *pool) {
    int n;
    if (pool == NULL) {
        return 0;
    }
    pthread_mutex_lock(&pool->stats_mutex);
    n = pool->total_processed;
    pthread_mutex_unlock(&pool->stats_mutex);
    return n;
}

int thread_pool_get_errors(thread_pool_t *pool) {
    int n;
    if (pool == NULL) {
        return 0;
    }
    pthread_mutex_lock(&pool->stats_mutex);
    n = pool->total_errors;
    pthread_mutex_unlock(&pool->stats_mutex);
    return n;
}
