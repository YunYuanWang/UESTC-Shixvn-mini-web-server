/*
 * request_worker.c — 请求处理工作进程
 *
 * 由 process_server 通过 fork() + execl() 启动。
 * 每个 worker 独立处理一个 .req 文件，生成对应的 .out 输出。
 * 支持一个 .req 文件中包含多行请求。
 *
 * 命令行:
 *   ./request_worker <semid> <log_path> <csv_path> <req_path> <out_path>
 */

#include "../include/ipc_utils.h"
#include "../include/log.h"
#include "../include/request_handler.h"
#include "../include/user_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int   semid;
    const char *log_path;
    const char *csv_path;
    const char *req_path;
    const char *out_path;

    FILE *fp;
    char  line[512];
    FILE *out_fp;
    int   request_count = 0;

    /* ---- parse command line ---- */
    if (argc != 6) {
        fprintf(stderr,
                "Usage: %s <semid> <log_path> <csv_path>"
                " <req_path> <out_path>\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }

    semid    = atoi(argv[1]);
    log_path = argv[2];
    csv_path = argv[3];
    req_path = argv[4];
    out_path = argv[5];

    /* ---- initialize log ---- */
    if (log_init_single(log_path) != 0) {
        fprintf(stderr, "[Worker] failed to open log file: %s\n", log_path);
        exit(EXIT_FAILURE);
    }

    /* ---- load user data ---- */
    user_store_load_csv(csv_path);

    mutex_wait(semid);
    log_info_pid(getpid(), "[Worker] processing request");
    mutex_signal(semid);

    /* ---- open request file ---- */
    fp = fopen(req_path, "r");
    if (fp == NULL) {
        mutex_wait(semid);
        log_error_pid(getpid(), "[Worker] cannot open request file");
        mutex_signal(semid);
        log_close();
        user_store_free();
        exit(EXIT_FAILURE);
    }

    /* ---- open output file (truncate) ---- */
    out_fp = fopen(out_path, "w");
    if (out_fp == NULL) {
        mutex_wait(semid);
        log_error_pid(getpid(), "[Worker] cannot write output file");
        mutex_signal(semid);
        fclose(fp);
        log_close();
        user_store_free();
        exit(EXIT_FAILURE);
    }

    /* ---- write connection info header ---- */
    fprintf(out_fp,
        "=== Connection Info ===\n"
        "Client IP: 127.0.0.1\n"
        "Client Port: 54321\n"
        "Server: MiniWeb\n"
        "=======================\n");

    /* ---- process each line of the request file ---- */
    while (fgets(line, sizeof(line), fp) != NULL) {
        request_t req;
        char output[4096];

        /* strip trailing newline */
        line[strcspn(line, "\r\n")] = '\0';

        /* skip blank lines */
        if (line[0] == '\0') {
            continue;
        }

        /* parse the request line */
        if (request_handler_parse(line, &req) != 0) {
            mutex_wait(semid);
            log_error_pid(getpid(), "[Worker] malformed request");
            mutex_signal(semid);
            continue;   /* skip this line, continue with next */
        }

        /* for POST: read next line as body */
        if (strcmp(req.method, "POST") == 0) {
            char body[512];
            if (fgets(body, sizeof(body), fp) != NULL) {
                body[strcspn(body, "\r\n")] = '\0';
                request_handler_set_body(body, &req);
            }
        }

        /* generate response (without connection header — already written) */
        if (request_handler_process(&req, output, sizeof(output)) < 0) {
            mutex_wait(semid);
            log_error_pid(getpid(), "[Worker] request processing failed");
            mutex_signal(semid);
            continue;
        }

        /* delimiter between requests */
        if (request_count > 0) {
            fprintf(out_fp, "---\n");
        }

        /* write response body */
        fprintf(out_fp, "%s", output);

        request_count++;
    }

    fclose(fp);
    fclose(out_fp);

    mutex_wait(semid);
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "[Worker] request processed (%d lines)",
                 request_count);
        log_info_pid(getpid(), msg);
    }
    mutex_signal(semid);

    /* ---- cleanup ---- */
    log_close();
    user_store_free();
    exit(EXIT_SUCCESS);
}
