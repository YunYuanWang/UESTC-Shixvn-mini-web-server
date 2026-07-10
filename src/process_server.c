/*
 * process_server.c — 多进程请求处理启动器
 *
 * 扫描 requests/*.req，为每个请求 fork 子进程，
 * 通过 execl() 启动 request_worker 处理请求，
 * 等待所有子进程结束并记录结果。
 *
 * 架构（参照 sday04 的 fork+exec 模式）:
 *   mini_web_server process
 *       +-- fork() + execl("./request_worker", semid, ...)
 *             |-- worker: hello.req    -> outputs/hello.out
 *             |-- worker: user_find.req -> outputs/user_find.out
 *             +-- worker: missing.req   -> outputs/missing.out
 */

#include "../include/ipc_utils.h"
#include "../include/log.h"
#include "../include/process_server.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_REQ_FILES   64
#define SEM_PROJ_ID     'W'   /* ftok project id for semaphore key */

/* ================================================================
 *  process_server_run
 *
 *  启动器主函数:
 *    1. 创建 outputs 目录
 *    2. 创建 System V 信号量（用于日志同步）
 *    3. 扫描 requests/ 目录，对每个 .req 文件 fork + execl
 *    4. waitpid 等待所有子进程结束
 *    5. 清理 IPC 资源
 *
 *  父进程（启动器）不使用信号量，只负责创建和销毁。
 *  子进程（request_worker）使用信号量保护并发日志写入。
 * ================================================================ */
int process_server_run(void) {
    DIR *dir = NULL;
    key_t sem_key;
    int   semid = -1;
    pid_t child_pids[MAX_REQ_FILES];
    int   child_count = 0;
    int   ret = -1;
    char  semid_str[16];

    /* ---- create outputs directory ---- */
    if (mkdir("outputs", 0755) != 0 && errno != EEXIST) {
        perror("[ProcessServer] mkdir outputs");
        return -1;
    }

    /* ---- create semaphore for log synchronisation ---- */
    sem_key = ftok(".", SEM_PROJ_ID);
    if (sem_key == -1) {
        perror("[ProcessServer] ftok");
        return -1;
    }

    /* clean up any stale semaphore set from a previous run */
    {
        int stale = semget(sem_key, 1, 0);
        if (stale != -1) {
            semctl(stale, 0, IPC_RMID);
        }
    }

    semid = semget(sem_key, 1, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("[ProcessServer] semget");
        return -1;
    }

    /* initialise mutex semaphore to 1 */
    if (semctl(semid, SEM_LOG_MUTEX, SETVAL, 1) == -1) {
        perror("[ProcessServer] semctl SETVAL");
        goto cleanup_sem;
    }

    /* convert semid to string for execl */
    snprintf(semid_str, sizeof(semid_str), "%d", semid);

    /* ---- open requests directory ---- */
    dir = opendir("requests");
    if (dir == NULL) {
        perror("[ProcessServer] opendir requests");
        goto cleanup_sem;
    }

    log_info_pid(getpid(), "[ProcessServer] scanning requests");

    /* ============================================================
     *  phase 1 — scan and fork + execl
     * ============================================================ */
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            const char *ext;
            pid_t pid;
            char req_path[512];
            char out_path[512];
            char *basename_copy;
            char *dot;
            char msg[512];

            /* filter *.req files only */
            ext = strrchr(name, '.');
            if (ext == NULL || strcmp(ext, ".req") != 0) {
                continue;
            }

            if (child_count >= MAX_REQ_FILES) {
                log_error_pid(getpid(),
                    "[ProcessServer] too many request files");
                break;
            }

            snprintf(req_path, sizeof(req_path), "requests/%s", name);

            /* build output and log paths: replace .req suffix */
            basename_copy = strdup(name);
            if (basename_copy == NULL) {
                log_error_pid(getpid(), "[ProcessServer] strdup failed");
                continue;
            }
            dot = strrchr(basename_copy, '.');
            if (dot != NULL) {
                *dot = '\0';
            }
            snprintf(out_path, sizeof(out_path), "outputs/%s.out",
                     basename_copy);
            free(basename_copy);

            pid = fork();
            if (pid < 0) {
                perror("[ProcessServer] fork");
                continue;
            }

            if (pid == 0) {
                /* ================================================
                 *  child - exec request_worker
                 * ================================================ */
                closedir(dir);
                execl("./request_worker",
                      "./request_worker",
                      semid_str,
                      "logs/server.log",
                      "data/users.csv",
                      req_path,
                      out_path,
                      NULL);

                perror("[ProcessServer] execl ./request_worker failed");
                exit(EXIT_FAILURE);
            }

            /* ---- parent ---- */
            child_pids[child_count] = pid;
            child_count++;

            {
                char banner[512];
                snprintf(banner, sizeof(banner),
                         "====== Child %d started [%s] ======",
                         (int)pid, name);
                log_info_pid(pid, banner);
            }
        }
    }

    closedir(dir);
    dir = NULL;

    /* ============================================================
     *  phase 2 — reap children
     * ============================================================ */
    for (int i = 0; i < child_count; i++) {
        int status;
        pid_t done = waitpid(-1, &status, 0);
        if (done > 0) {
            char msg[128];
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                snprintf(msg, sizeof(msg),
                         "[ProcessServer] child %d exited with status %d",
                         (int)done, exit_code);
                log_info_pid(done, msg);
            } else {
                snprintf(msg, sizeof(msg),
                         "[ProcessServer] child %d terminated abnormally",
                         (int)done);
                log_error_pid(done, msg);
            }
        }
    }

    log_info_pid(getpid(), "[ProcessServer] all children processed");
    ret = 0;

    /* ---- cleanup ---- */
cleanup_sem:
    if (semid >= 0) {
        semctl(semid, 0, IPC_RMID);
    }
    return ret;
}
