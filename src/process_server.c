/*
 * process_server.c — 多线程请求处理启动器
 *
 * 父线程扫描 requests 目录下的 .req 文件，将任务路径加入共享队列，
 * 创建多个 worker 线程从队列中取任务并处理。
 *
 * 架构（POSIX 多线程，风格参考 os_course）:
 *   mini_web_server process (主线程)
 *       +-- parent: 扫描 requests/ → 入队 → 创建 workers
 *       +-- worker[0]: 从队列取任务 → 处理 → outputs/
 *       +-- worker[1]: 从队列取任务 → 处理 → outputs/
 *       +-- worker[N]: 从队列取任务 → 处理 → outputs/
 *
 * 同步机制:
 *   - sem_t tasks_sem:           计数信号量，计数可用任务数
 *   - pthread_mutex_t queue_mutex: 保护请求队列
 *   - pthread_cond_t  queue_cond:  条件变量，队列非空时通知 worker
 *   - pthread_mutex_t stats_mutex: 保护统计数据
 *   - pthread_mutex_t log_mutex:   保护日志写入
 */

#include "../include/log.h"
#include "../include/process_server.h"
#include "../include/request_handler.h"
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_REQ_FILES   64
#define NUM_WORKERS     4

/* ================================================================
 *  shared data — 请求队列 + 终止标志
 * ================================================================ */
static char request_queue[MAX_REQ_FILES][512];
static int  queue_count = 0;
static int  queue_head  = 0;
static int  queue_tail  = 0;
static int  done_flag   = 0;   /* 1 = 所有任务已入队，worker 可安全退出 */

/* ================================================================
 *  synchronization primitives（POSIX 风格，参照 os_course）
 * ================================================================ */
static sem_t            tasks_sem;    /* 计数信号量: 可用任务数 */
static pthread_mutex_t  queue_mutex;  /* 互斥量: 保护请求队列 */
static pthread_cond_t   queue_cond;   /* 条件变量: 队列非空 */
static pthread_mutex_t  stats_mutex;  /* 互斥量: 保护统计数据 */
static pthread_mutex_t  log_mutex;    /* 互斥量: 保护日志写入 */

/* ================================================================
 *  statistics
 * ================================================================ */
static int total_processed = 0;
static int total_errors    = 0;

/* ================================================================
 *  thread-safe log wrappers
 * ================================================================ */
static void log_ts(const char *msg) {
    pthread_mutex_lock(&log_mutex);
    log_info(msg);
    pthread_mutex_unlock(&log_mutex);
}

static void log_error_ts(const char *msg) {
    pthread_mutex_lock(&log_mutex);
    log_error(msg);
    pthread_mutex_unlock(&log_mutex);
}

/* ================================================================
 *  worker_thread_func — 工作线程入口
 *
 *  循环: sem_wait → lock queue → dequeue → unlock → process → stats
 *  退出: queue 为空 且 done_flag == 1 时 return
 * ================================================================ */
static void *worker_thread_func(void *arg) {
    int worker_id = *(int *)arg;
    free(arg);

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "[Worker-%d] started", worker_id);
        log_ts(msg);
    }

    while (1) {
        char req_path[512];
        char out_path[512];
        char *basename_copy;
        char *dot;
        const char *fname;

        /* ---- 信号量等待任务（POSIX sem_wait） ---- */
        sem_wait(&tasks_sem);

        /* ---- 从共享队列取任务（互斥量保护） ---- */
        pthread_mutex_lock(&queue_mutex);

        /* 条件变量等待：队列为空时阻塞，防止虚假唤醒 */
        while (queue_count == 0) {
            if (done_flag) {
                /* 终止信号：无更多任务 */
                pthread_mutex_unlock(&queue_mutex);
                {
                    char msg[64];
                    snprintf(msg, sizeof(msg),
                             "[Worker-%d] exiting (no more tasks)", worker_id);
                    log_ts(msg);
                }
                return NULL;
            }
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }

        /* 出队 */
        strncpy(req_path, request_queue[queue_head], sizeof(req_path) - 1);
        req_path[sizeof(req_path) - 1] = '\0';
        queue_head = (queue_head + 1) % MAX_REQ_FILES;
        queue_count--;

        pthread_mutex_unlock(&queue_mutex);

        /* ---- 处理请求 ---- */
        {
            FILE *fp;
            FILE *out_fp;
            char line[512];
            int request_count = 0;
            int errors = 0;

            /* 提取文件名，如 requests/hello.req → hello */
            fname = strrchr(req_path, '/');
            fname = (fname != NULL) ? fname + 1 : req_path;

            basename_copy = strdup(fname);
            if (basename_copy == NULL) {
                log_error_ts("[Worker] strdup failed");
                pthread_mutex_lock(&stats_mutex);
                total_errors++;
                pthread_mutex_unlock(&stats_mutex);
                continue;
            }
            dot = strrchr(basename_copy, '.');
            if (dot != NULL) {
                *dot = '\0';
            }

            /* 构建输出路径: outputs/<basename>.out */
            snprintf(out_path, sizeof(out_path), "outputs/%s.out",
                     basename_copy);
            free(basename_copy);

            {
                char msg[640];
                snprintf(msg, sizeof(msg),
                         "[Worker-%d] processing %s", worker_id, fname);
                log_ts(msg);
            }

            /* 打开请求文件 */
            fp = fopen(req_path, "r");
            if (fp == NULL) {
                char msg[640];
                snprintf(msg, sizeof(msg),
                         "[Worker-%d] cannot open request: %s",
                         worker_id, req_path);
                log_error_ts(msg);
                pthread_mutex_lock(&stats_mutex);
                total_errors++;
                pthread_mutex_unlock(&stats_mutex);
                continue;
            }

            /* 打开输出文件 */
            out_fp = fopen(out_path, "w");
            if (out_fp == NULL) {
                char msg[640];
                snprintf(msg, sizeof(msg),
                         "[Worker-%d] cannot write output: %s",
                         worker_id, out_path);
                log_error_ts(msg);
                fclose(fp);
                pthread_mutex_lock(&stats_mutex);
                total_errors++;
                pthread_mutex_unlock(&stats_mutex);
                continue;
            }

            /* 写连接信息头 */
            fprintf(out_fp,
                "=== Connection Info ===\n"
                "Client IP: 127.0.0.1\n"
                "Client Port: 54321\n"
                "Server: MiniWeb\n"
                "=======================\n");

            /* 逐行处理请求 */
            while (fgets(line, sizeof(line), fp) != NULL) {
                request_t req;
                char output[4096];

                /* 去掉换行符 */
                line[strcspn(line, "\r\n")] = '\0';

                /* 跳过空行 */
                if (line[0] == '\0') {
                    continue;
                }

                /* 解析请求行 */
                if (request_handler_parse(line, &req) != 0) {
                    char msg[640];
                    snprintf(msg, sizeof(msg),
                             "[Worker-%d] malformed request: %s",
                             worker_id, line);
                    log_error_ts(msg);
                    continue;
                }

                /* POST: 读取下一行作为 body */
                if (strcmp(req.method, "POST") == 0) {
                    char body[512];
                    if (fgets(body, sizeof(body), fp) != NULL) {
                        body[strcspn(body, "\r\n")] = '\0';
                        request_handler_set_body(body, &req);
                    }
                }

                /* 生成响应 */
                if (request_handler_process(&req, output, sizeof(output)) < 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "[Worker-%d] request processing failed",
                             worker_id);
                    log_error_ts(msg);
                    errors++;
                    continue;
                }

                /* 请求之间加分隔线 */
                if (request_count > 0) {
                    fprintf(out_fp, "---\n");
                }

                fprintf(out_fp, "%s", output);
                request_count++;
            }

            fclose(fp);
            fclose(out_fp);

            /* ---- 更新统计数据（互斥量保护） ---- */
            pthread_mutex_lock(&stats_mutex);
            total_processed++;
            if (errors > 0) {
                total_errors += errors;
            }
            pthread_mutex_unlock(&stats_mutex);

            {
                char msg[640];
                snprintf(msg, sizeof(msg),
                         "[Worker-%d] done with %s (%d lines, %d errors)",
                         worker_id, fname, request_count, errors);
                log_ts(msg);
            }
        }
    }

    return NULL;
}

/* ================================================================
 *  process_server_run — 启动器主函数
 *
 *  流程:
 *    1. 创建 outputs 目录
 *    2. 初始化 POSIX 同步原语（sem_t, mutex, cond）
 *    3. 扫描 requests/ 目录，将任务路径加入队列
 *    4. 设置 done_flag，发送终止信号量
 *    5. 创建 NUM_WORKERS 个工作线程
 *    6. pthread_join 等待所有 worker 结束
 *    7. 输出统计信息，清理同步资源
 * ================================================================ */
int process_server_run(void) {
    DIR *dir = NULL;
    pthread_t worker_ids[MAX_REQ_FILES];
    int worker_count = 0;
    int ret = -1;

    /* ---- create outputs directory ---- */
    if (mkdir("outputs", 0755) != 0 && errno != EEXIST) {
        perror("[ProcessServer] mkdir outputs");
        return -1;
    }

    /* ---- init POSIX synchronization primitives ---- */
    if (sem_init(&tasks_sem, 0, 0) != 0) {
        perror("[ProcessServer] sem_init");
        return -1;
    }

    if (pthread_mutex_init(&queue_mutex, NULL) != 0) {
        perror("[ProcessServer] pthread_mutex_init(queue)");
        sem_destroy(&tasks_sem);
        return -1;
    }

    if (pthread_cond_init(&queue_cond, NULL) != 0) {
        perror("[ProcessServer] pthread_cond_init");
        pthread_mutex_destroy(&queue_mutex);
        sem_destroy(&tasks_sem);
        return -1;
    }

    if (pthread_mutex_init(&stats_mutex, NULL) != 0) {
        perror("[ProcessServer] pthread_mutex_init(stats)");
        pthread_cond_destroy(&queue_cond);
        pthread_mutex_destroy(&queue_mutex);
        sem_destroy(&tasks_sem);
        return -1;
    }

    if (pthread_mutex_init(&log_mutex, NULL) != 0) {
        perror("[ProcessServer] pthread_mutex_init(log)");
        pthread_mutex_destroy(&stats_mutex);
        pthread_cond_destroy(&queue_cond);
        pthread_mutex_destroy(&queue_mutex);
        sem_destroy(&tasks_sem);
        return -1;
    }

    /* ---- open requests directory ---- */
    dir = opendir("requests");
    if (dir == NULL) {
        perror("[ProcessServer] opendir requests");
        goto cleanup;
    }

    log_info("[ProcessServer] scanning requests (multi-thread mode)");

    /* ================================================================
     *  phase 1 — 父线程扫描 requests/，将任务路径加入共享队列
     * ================================================================ */
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            const char *ext;
            char req_path[512];

            /* 过滤 *.req 文件 */
            ext = strrchr(name, '.');
            if (ext == NULL || strcmp(ext, ".req") != 0) {
                continue;
            }

            if (queue_count >= MAX_REQ_FILES) {
                log_error("[ProcessServer] too many request files, "
                          "queue full");
                break;
            }

            snprintf(req_path, sizeof(req_path), "requests/%s", name);

            /* ---- 入队（互斥量保护） ---- */
            pthread_mutex_lock(&queue_mutex);
            strncpy(request_queue[queue_tail], req_path,
                    sizeof(request_queue[queue_tail]) - 1);
            request_queue[queue_tail][sizeof(request_queue[queue_tail]) - 1] = '\0';
            queue_tail = (queue_tail + 1) % MAX_REQ_FILES;
            queue_count++;

            /* 通知等待的 worker: 有新任务 */
            sem_post(&tasks_sem);               /* 信号量 +1 */
            pthread_cond_signal(&queue_cond);   /* 条件变量唤醒 */

            pthread_mutex_unlock(&queue_mutex);

            {
                char msg[640];
                snprintf(msg, sizeof(msg),
                         "[ProcessServer] enqueued: %s", name);
                log_info(msg);
            }
        }
    }

    closedir(dir);
    dir = NULL;

    /* ---- 设置终止标志 + 发送终止信号量 ---- */
    pthread_mutex_lock(&queue_mutex);
    done_flag = 1;
    pthread_mutex_unlock(&queue_mutex);

    /* 每个 worker 一个终止信号，确保都能被唤醒检查 done_flag */
    for (int i = 0; i < NUM_WORKERS; i++) {
        sem_post(&tasks_sem);
    }
    /* 广播条件变量，唤醒所有可能在等待的 worker */
    pthread_cond_broadcast(&queue_cond);

    {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "[ProcessServer] queue filled (%d tasks), "
                 "spawning %d workers",
                 queue_count, NUM_WORKERS);
        log_info(msg);
    }

    /* ================================================================
     *  phase 2 — 创建工作线程
     * ================================================================ */
    for (int i = 0; i < NUM_WORKERS; i++) {
        int *id = malloc(sizeof(int));
        if (id == NULL) {
            log_error("[ProcessServer] malloc for worker id failed");
            continue;
        }
        *id = i + 1;   /* worker 编号从 1 开始 */

        if (pthread_create(&worker_ids[worker_count], NULL,
                           worker_thread_func, id) != 0) {
            perror("[ProcessServer] pthread_create");
            free(id);
            continue;
        }
        worker_count++;
    }

    /* ================================================================
     *  phase 3 — pthread_join 等待所有 worker 结束
     * ================================================================ */
    for (int i = 0; i < worker_count; i++) {
        pthread_join(worker_ids[i], NULL);
        {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "[ProcessServer] worker thread %d joined", i + 1);
            log_info(msg);
        }
    }

    /* ---- 输出统计 ---- */
    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "[ProcessServer] all workers done — "
                 "processed: %d, errors: %d",
                 total_processed, total_errors);
        log_info(msg);
    }

    ret = 0;

    /* ---- 清理同步资源 ---- */
cleanup:
    pthread_mutex_destroy(&log_mutex);
    pthread_mutex_destroy(&stats_mutex);
    pthread_cond_destroy(&queue_cond);
    pthread_mutex_destroy(&queue_mutex);
    sem_destroy(&tasks_sem);

    return ret;
}
