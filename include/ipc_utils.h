#ifndef IPC_UTILS_H
#define IPC_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/types.h>

/* ---- semaphore indices ---- */
#define SEM_LOG_MUTEX 0

/*
 * sem_raw - lower-level semop wrapper.
 * Calls exit(EXIT_FAILURE) on failure (child process context).
 */
static inline void sem_raw(int semid, int sem_num, int op, int undo_flag) {
    struct sembuf sops;
    sops.sem_num = (unsigned short)sem_num;
    sops.sem_op  = (short)op;
    sops.sem_flg = (short)(undo_flag ? SEM_UNDO : 0);
    if (semop(semid, &sops, 1) == -1) {
        perror("semop");
        exit(EXIT_FAILURE);
    }
}

/*
 * sem_wait - decrement counting semaphore (no SEM_UNDO).
 */
static inline void sem_wait(int semid, int sem_num) {
    sem_raw(semid, sem_num, -1, 0);
}

/*
 * sem_signal - increment counting semaphore (no SEM_UNDO).
 */
static inline void sem_signal(int semid, int sem_num) {
    sem_raw(semid, sem_num, 1, 0);
}

/*
 * mutex_wait - acquire mutex (with SEM_UNDO, auto-release on crash).
 */
static inline void mutex_wait(int semid) {
    sem_raw(semid, SEM_LOG_MUTEX, -1, 1);
}

/*
 * mutex_signal - release mutex (with SEM_UNDO).
 */
static inline void mutex_signal(int semid) {
    sem_raw(semid, SEM_LOG_MUTEX, 1, 1);
}

#endif
