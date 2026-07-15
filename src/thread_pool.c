/*
 * thread_pool.c — dynamic thread pool with core/max sizing
 *
 * Architecture:
 *   - Circular buffer of task_t (each wrapping a client_fd).
 *   - Mutex + two condition variables (not_empty / not_full) guard the queue.
 *   - Starts with core_size workers; scales up to max_size on demand.
 *   - Extra workers exit after IDLE_TIMEOUT_MS of inactivity.
 *
 * Scale-up trigger:
 *   After enqueuing a task, if the queue is non-empty (count > 0) and
 *   we haven't reached max_size, spawn a new worker.  count > 0 after
 *   enqueue means no idle worker was available to pick it up immediately.
 *
 * Scale-down trigger:
 *   Extra workers (is_core == 0) use pthread_cond_timedwait instead of
 *   pthread_cond_wait.  If the timeout expires with the queue still empty,
 *   they exit, decrementing num_workers.  Core workers wait indefinitely.
 */

#include "../include/thread_pool.h"
#include "../include/log.h"
#include "../include/request_handler.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- forward declaration ---- */
static void *worker_thread_func(void *arg);

/* ---- helpers ---- */

static void timespec_add_ms(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/*
 * Internal: spawn a new worker thread.
 * Called while pool->mutex is NOT held.
 * Returns 0 on success, -1 on failure.
 */
static int spawn_worker(thread_pool_t *pool, int is_core) {
    int slot;
    worker_arg_t *arg;

    /* ---- find a free slot and update state under mutex ---- */
    pthread_mutex_lock(&pool->mutex);

    for (slot = 0; slot < pool->max_size; slot++) {
        if (!pool->worker_active[slot]) {
            break;
        }
    }
    if (slot >= pool->max_size) {
        pthread_mutex_unlock(&pool->mutex);
        return -1;  /* no free slots */
    }

    pool->worker_active[slot] = 1;
    pool->num_workers++;

    pthread_mutex_unlock(&pool->mutex);

    /* update peak (separate lock) */
    pthread_mutex_lock(&pool->stats_mutex);
    if (pool->num_workers > pool->peak_workers) {
        pool->peak_workers = pool->num_workers;
    }
    pthread_mutex_unlock(&pool->stats_mutex);

    /* ---- create the thread outside any lock ---- */
    arg = (worker_arg_t *)malloc(sizeof(*arg));
    if (arg == NULL) {
        /* rollback */
        pthread_mutex_lock(&pool->mutex);
        pool->worker_active[slot] = 0;
        pool->num_workers--;
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }

    arg->pool    = pool;
    arg->slot    = slot;
    arg->is_core = is_core;

    if (pthread_create(&pool->workers[slot], NULL, worker_thread_func, arg) != 0) {
        /* rollback */
        pthread_mutex_lock(&pool->mutex);
        pool->worker_active[slot] = 0;
        pool->num_workers--;
        pthread_mutex_unlock(&pool->mutex);
        free(arg);
        return -1;
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[ThreadPool] scaled up — worker spawned (slot %d, %s, now %d workers)",
                 slot, is_core ? "core" : "extra", pool->num_workers);
        log_info(msg);
    }

    return 0;
}

/* ================================================================
 *  worker thread function
 * ================================================================ */
static void *worker_thread_func(void *arg) {
    worker_arg_t  *warg = (worker_arg_t *)arg;
    thread_pool_t *pool = warg->pool;
    int            slot = warg->slot;
    int            is_core = warg->is_core;
    char           msg[256];

    free(warg);  /* we've copied the fields we need */

    /* ---- assign worker label for request_handler log messages ---- */
    {
        /*
         * We use the slot index as a stable worker number.  Core workers
         * occupy slots 0..core_size-1 initially; extra workers fill higher
         * slots.  Slot numbers are reused after a worker exits.
         */
        static pthread_mutex_t label_mutex = PTHREAD_MUTEX_INITIALIZER;
        static int next_label = 0;
        pthread_mutex_lock(&label_mutex);
        next_label++;
        int my_label = next_label;
        pthread_mutex_unlock(&label_mutex);

        request_handler_set_worker_label(my_label);

        snprintf(msg, sizeof(msg),
                 "[Worker-%d] started (slot %d, %s)",
                 my_label, slot, is_core ? "core" : "extra");
        log_info(msg);
    }

    while (1) {
        int client_fd;

        /* ---- wait for a task (or idle-timeout for extra workers) ---- */
        pthread_mutex_lock(&pool->mutex);

        while (pool->count == 0 && !pool->shutdown) {
            if (!is_core) {
                /* extra worker — timed wait */
                struct timespec ts;
                int rc;

                timespec_add_ms(&ts, IDLE_TIMEOUT_MS);
                rc = pthread_cond_timedwait(&pool->not_empty,
                                            &pool->mutex, &ts);
                if (rc == ETIMEDOUT && pool->count == 0 && !pool->shutdown) {
                    /* idle timeout — scale down */
                    pool->worker_active[slot] = 0;
                    pool->num_workers--;
                    pthread_mutex_unlock(&pool->mutex);

                    snprintf(msg, sizeof(msg),
                             "[Worker-%d] idle timeout — scaling down "
                             "(slot %d released, now %d workers)",
                             request_handler_get_worker_label_int(),
                             slot, pool->num_workers);
                    log_info(msg);
                    return NULL;
                }
                /* if rc == ETIMEDOUT but count > 0, we were raced —
                 * fall through to dequeue */
            } else {
                /* core worker — indefinite wait */
                pthread_cond_wait(&pool->not_empty, &pool->mutex);
            }
        }

        /* exit condition: shutdown AND queue drained */
        if (pool->shutdown && pool->count == 0) {
            pool->worker_active[slot] = 0;
            pool->num_workers--;
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
                     request_handler_get_worker_label_int(), client_fd);
            log_info(msg);

            ret = request_handler_handle_connection(client_fd);

            /* worker is responsible for closing the client fd */
            close(client_fd);

            snprintf(msg, sizeof(msg),
                     "[Worker-%d] connection closed (fd=%d)",
                     request_handler_get_worker_label_int(), client_fd);
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

    snprintf(msg, sizeof(msg),
             "[Worker-%d] exiting (slot %d)",
             request_handler_get_worker_label_int(), slot);
    log_info(msg);

    return NULL;
}

/* ================================================================
 *  public API
 * ================================================================ */

thread_pool_t *thread_pool_create(int core_size, int max_size,
                                  int queue_capacity) {
    thread_pool_t *pool;
    int i;

    if (core_size < 1 || max_size < core_size || queue_capacity < 1) {
        return NULL;
    }
    if (max_size > MAX_POOL_WORKERS) {
        max_size = MAX_POOL_WORKERS;
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

    pool->capacity    = queue_capacity;
    pool->count       = 0;
    pool->head        = 0;
    pool->tail        = 0;
    pool->core_size   = core_size;
    pool->max_size    = max_size;
    pool->num_workers = 0;
    pool->shutdown    = 0;
    pool->peak_workers = 0;

    /* init sync primitives */
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);
    pthread_mutex_init(&pool->stats_mutex, NULL);

    /* spawn core workers */
    for (i = 0; i < core_size; i++) {
        if (spawn_worker(pool, 1 /* is_core */) != 0) {
            log_error("[ThreadPool] failed to spawn core worker");
            /* shut down any workers already created */
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->not_empty);
            thread_pool_destroy(pool, NULL, NULL);
            return NULL;
        }
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[ThreadPool] created — core=%d max=%d queue_cap=%d",
                 core_size, max_size, queue_capacity);
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

    /*
     * Scale-up check: if the queue still has pending tasks after
     * signalling a worker, it means no idle worker was available
     * (all current workers are busy).  Decide inside the mutex,
     * then spawn outside it.
     */
    {
        int should_spawn = (pool->count > 0 &&
                            pool->num_workers < pool->max_size);
        pthread_mutex_unlock(&pool->mutex);

        if (should_spawn) {
            spawn_worker(pool, 0 /* is_core = extra */);
        }
    }

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

    /* join all active workers */
    for (i = 0; i < pool->max_size; i++) {
        if (pool->worker_active[i]) {
            pthread_join(pool->workers[i], NULL);
            {
                char msg[64];
                snprintf(msg, sizeof(msg),
                         "[ThreadPool] worker slot %d joined", i);
                log_info(msg);
            }
        }
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[ThreadPool] all workers finished — peak workers: %d",
                 pool->peak_workers);
        log_info(msg);
    }

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

int thread_pool_get_peak_workers(thread_pool_t *pool) {
    int n;
    if (pool == NULL) {
        return 0;
    }
    pthread_mutex_lock(&pool->stats_mutex);
    n = pool->peak_workers;
    pthread_mutex_unlock(&pool->stats_mutex);
    return n;
}
