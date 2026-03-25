/* =============================================================================
 * main.c — Line Controller (Parent Process)
 * =============================================================================
 * The top-level supervisor for the PCB Quality Inspection Line.
 *
 * Responsibilities:
 *   1. Create all IPC resources (FIFOs, shared memory, semaphore, msg queue)
 *   2. Initialize logger (binary log + alerts log)
 *   3. Fork and exec all child processes
 *   4. Install signal handlers (SIGINT, SIGALRM, SIGUSR1, SIGCHLD)
 *   5. Receive defect alerts from message queue
 *   6. Wait for all children to exit (waitpid)
 *   7. Clean up all IPC resources on shutdown
 *
 * System calls: fork, exec, waitpid, sigaction, alarm,
 *               mkfifo, shmget, shmat, shmdt, shmctl,
 *               sem_open, sem_close, sem_unlink,
 *               msgget, msgrcv, msgctl
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <semaphore.h>

#include "../include/board.h"
#include "../include/ipc_config.h"
#include "../include/logger.h"

/* ── Child process table ─────────────────────────────────── */
#define NUM_CHILDREN 5

typedef struct {
    const char *name;
    const char *exec_path;
    pid_t       pid;
    int         exited;
    int         exit_status;
} child_proc_t;

static child_proc_t g_children[NUM_CHILDREN] = {
    { "board_gen",  "./bin/board_gen",  0, 0, 0 },
    { "station1",   "./bin/station1",   0, 0, 0 },
    { "station2",   "./bin/station2",   0, 0, 0 },
    { "station3",   "./bin/station3",   0, 0, 0 },
    { "dashboard",  "./bin/dashboard",  0, 0, 0 },
};

/* ── Global IPC handles ──────────────────────────────────── */
static int      g_shm_id   = -1;
static stats_t *g_stats    = NULL;
static sem_t   *g_shm_sem  = SEM_FAILED;
static int      g_msgq_id  = -1;
static logger_ctx_t g_log;

/* ── Shutdown flag ───────────────────────────────────────── */
static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_snapshot = 0;
static volatile sig_atomic_t g_alarm_tick = 0;
static volatile sig_atomic_t g_child_died = 0;

/* ── Signal Handlers ─────────────────────────────────────── */

/*
 * SIGINT — graceful shutdown
 * Sets g_shutdown flag; main loop will forward SIGTERM to all children
 */
static void handle_sigint(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/*
 * SIGALRM — periodic throughput ticker (every SIM_ALARM_INTERVAL_SEC)
 * Re-arms itself automatically
 */
static void handle_sigalrm(int sig)
{
    (void)sig;
    g_alarm_tick = 1;
    alarm(SIM_ALARM_INTERVAL_SEC); /* re-arm */
}

/*
 * SIGUSR1 — snapshot request
 * Sets g_snapshot flag; main loop will dump stats to file
 */
static void handle_sigusr1(int sig)
{
    (void)sig;
    g_snapshot = 1;
}

/*
 * SIGCHLD — child process exited
 * Sets flag; main loop uses waitpid(WNOHANG) to reap
 */
static void handle_sigchld(int sig)
{
    (void)sig;
    g_child_died = 1;
}

/* ── Signal Installation ─────────────────────────────────── */

static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = handle_sigint;
    if (sigaction(SIGINT,  &sa, NULL) < 0) { SYS_ERR("sigaction SIGINT");  return -1; }

    sa.sa_handler = handle_sigalrm;
    if (sigaction(SIGALRM, &sa, NULL) < 0) { SYS_ERR("sigaction SIGALRM"); return -1; }

    sa.sa_handler = handle_sigusr1;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) { SYS_ERR("sigaction SIGUSR1"); return -1; }

    sa.sa_handler = handle_sigchld;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) { SYS_ERR("sigaction SIGCHLD"); return -1; }

    /* SIGUSR2 is forwarded to children — ignore in parent */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags   = 0;
    sigaction(SIGUSR2, &sa, NULL);

    return 0;
}

/* ── IPC Setup ───────────────────────────────────────────── */

static int create_fifos(void)
{
    const char *paths[] = FIFO_PATHS;
    for (int i = 0; i < NUM_FIFOS; i++) {
        if (mkfifo(paths[i], FIFO_PERMS) < 0 && errno != EEXIST) {
            fprintf(stderr, "[CTRL] mkfifo %s failed: %s\n",
                    paths[i], strerror(errno));
            return -1;
        }
        printf("[%s] FIFO created: %s\n", ROLE_CONTROLLER, paths[i]);
    }
    return 0;
}

static int create_shared_memory(void)
{
    key_t key = GET_SHM_KEY();
    if (key < 0) { SYS_ERR("ftok shm"); return -1; }

    g_shm_id = shmget(key, sizeof(stats_t),
                      IPC_CREAT | IPC_EXCL | SHM_PERMS);
    if (g_shm_id < 0) {
        if (errno == EEXIST) {
            /* Already exists — attach to it */
            g_shm_id = shmget(key, sizeof(stats_t), SHM_PERMS);
        }
        if (g_shm_id < 0) { SYS_ERR("shmget"); return -1; }
    }

    g_stats = (stats_t *)shmat(g_shm_id, NULL, 0);
    if (g_stats == (void *)-1) { SYS_ERR("shmat"); return -1; }

    /* Initialize to zero */
    memset(g_stats, 0, sizeof(stats_t));
    printf("[%s] Shared memory created (id=%d, size=%zu bytes)\n",
           ROLE_CONTROLLER, g_shm_id, sizeof(stats_t));
    return 0;
}

static int create_semaphore(void)
{
    /* Remove stale semaphore if present */
    sem_unlink(SEM_SHM_NAME);

    g_shm_sem = sem_open(SEM_SHM_NAME, O_CREAT | O_EXCL,
                         0666, SEM_INITIAL_VAL);
    if (g_shm_sem == SEM_FAILED) { SYS_ERR("sem_open create"); return -1; }

    printf("[%s] Semaphore created: %s\n", ROLE_CONTROLLER, SEM_SHM_NAME);
    return 0;
}

static int create_message_queue(void)
{
    key_t key = GET_MSG_KEY();
    if (key < 0) { SYS_ERR("ftok msgq"); return -1; }

    g_msgq_id = msgget(key, IPC_CREAT | MSG_PERMS);
    if (g_msgq_id < 0) { SYS_ERR("msgget"); return -1; }

    printf("[%s] Message queue created (id=%d)\n",
           ROLE_CONTROLLER, g_msgq_id);
    return 0;
}

/* ── IPC Cleanup ─────────────────────────────────────────── */

static void cleanup_ipc(void)
{
    printf("[%s] Cleaning up IPC resources...\n", ROLE_CONTROLLER);

    /* Remove FIFOs */
    const char *paths[] = FIFO_PATHS;
    for (int i = 0; i < NUM_FIFOS; i++) {
        if (unlink(paths[i]) < 0 && errno != ENOENT)
            fprintf(stderr, "[CTRL] unlink %s: %s\n", paths[i], strerror(errno));
        else
            printf("[%s] Removed FIFO: %s\n", ROLE_CONTROLLER, paths[i]);
    }

    /* Detach and remove shared memory */
    if (g_stats && g_stats != (void *)-1) {
        shmdt(g_stats);
        g_stats = NULL;
    }
    if (g_shm_id >= 0) {
        shmctl(g_shm_id, IPC_RMID, NULL);
        printf("[%s] Shared memory removed\n", ROLE_CONTROLLER);
    }

    /* Close and unlink semaphore */
    if (g_shm_sem != SEM_FAILED) {
        sem_close(g_shm_sem);
        sem_unlink(SEM_SHM_NAME);
        printf("[%s] Semaphore removed: %s\n", ROLE_CONTROLLER, SEM_SHM_NAME);
    }

    /* Remove message queue */
    if (g_msgq_id >= 0) {
        msgctl(g_msgq_id, IPC_RMID, NULL);
        printf("[%s] Message queue removed\n", ROLE_CONTROLLER);
    }
}

/* ── Child Spawning ──────────────────────────────────────── */

static int spawn_child(child_proc_t *c)
{
    pid_t pid = fork();
    if (pid < 0) {
        SYS_ERR("fork");
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdout to run_output.log so dashboard stays clean */
        if (strcmp(c->name, "dashboard") != 0) {
            int log_fd = open("logs/run_output.log",
                              O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fd >= 0) {
                dup2(log_fd, STDOUT_FILENO);
                close(log_fd);
            }
        }
        /* exec the target binary */
        execl(c->exec_path, c->exec_path, (char *)NULL);
        fprintf(stderr, "[CTRL] execl %s failed: %s\n",
                c->exec_path, strerror(errno));
        _exit(EXIT_FIFO_ERROR);
    }

    /* Parent: record child PID */
    c->pid = pid;
    printf("[%s] Spawned %-12s (PID=%d)\n",
           ROLE_CONTROLLER, c->name, pid);
    return 0;
}

/* ── Child Reaping ───────────────────────────────────────── */

static void reap_children(void)
{
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < NUM_CHILDREN; i++) {
            if (g_children[i].pid == pid) {
                g_children[i].exited      = 1;
                g_children[i].exit_status = WEXITSTATUS(status);

                if (WIFEXITED(status)) {
                    printf("[%s] Child %-12s (PID=%d) exited normally (code=%d)\n",
                           ROLE_CONTROLLER,
                           g_children[i].name, pid,
                           WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("[%s] Child %-12s (PID=%d) KILLED by signal %d\n",
                           ROLE_CONTROLLER,
                           g_children[i].name, pid,
                           WTERMSIG(status));

                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "Child %s (PID=%d) killed by signal %d",
                             g_children[i].name, pid, WTERMSIG(status));
                    logger_write_event(&g_log, ROLE_CONTROLLER, msg);
                }
                break;
            }
        }
    }
}

/* ── Check if all children have exited ──────────────────── */

static int all_children_done(void)
{
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (!g_children[i].exited) return 0;
    }
    return 1;
}

/* ── Send signal to all children ────────────────────────── */

static void signal_all_children(int sig)
{
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (g_children[i].pid > 0 && !g_children[i].exited) {
            kill(g_children[i].pid, sig);
        }
    }
}

/* ── Wait for all children (blocking) ───────────────────── */

static void wait_all_children(void)
{
    printf("[%s] Waiting for all children to exit...\n", ROLE_CONTROLLER);
    for (int i = 0; i < NUM_CHILDREN; i++) {
        if (g_children[i].pid > 0 && !g_children[i].exited) {
            int status;
            waitpid(g_children[i].pid, &status, 0);
            g_children[i].exited      = 1;
            g_children[i].exit_status = WEXITSTATUS(status);
            printf("[%s] Child %-12s (PID=%d) joined\n",
                   ROLE_CONTROLLER,
                   g_children[i].name,
                   g_children[i].pid);
        }
    }
}

/* ── Alarm: Periodic Throughput Log ─────────────────────── */

static void log_throughput(void)
{
    if (!g_stats || !g_shm_sem) return;

    sem_wait(g_shm_sem);
    int completed    = g_stats->boards_completed;
    int total_defects = g_stats->total_defects;
    float rate       = g_stats->defect_rate_percent;
    sem_post(g_shm_sem);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "ALARM tick | completed=%d | defects=%d | rate=%.2f%%",
             completed, total_defects, rate);

    printf("[%s] %s\n", ROLE_CONTROLLER, msg);
    logger_write_event(&g_log, ROLE_CONTROLLER, msg);
}

/* ── Message Queue: Alert Receiver ──────────────────────── */

static void drain_message_queue(void)
{
    if (g_msgq_id < 0) return;

    alert_msg_t alert;
    while (1) {
        ssize_t rc = msgrcv(g_msgq_id, &alert,
                            sizeof(alert_msg_t) - sizeof(long),
                            0, IPC_NOWAIT);
        if (rc < 0) {
            if (errno == ENOMSG) break; /* Queue empty */
            SYS_ERR("msgrcv");
            break;
        }

        if (alert.msg_type == MSG_TYPE_DONE) {
            printf("[%s] Received DONE sentinel from Station 3\n",
                   ROLE_CONTROLLER);
            /* Don't shutdown yet — wait for all children to exit naturally */
            break;
        }

        printf("[%s] ALERT received | Board %-10s | %s\n",
               ROLE_CONTROLLER,
               alert.board_name,
               alert.description);
    }
}

/* ── Print Final Summary ─────────────────────────────────── */

static void print_summary(void)
{
    log_header_t hdr;
    if (logger_read_header(&g_log, &hdr) < 0) return;

    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║       FINAL INSPECTION SUMMARY           ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  Total Boards Logged : %-5d             ║\n", hdr.total_boards);
    printf("║  Passed              : %-5d             ║\n", hdr.total_pass);
    printf("║  Failed              : %-5d             ║\n", hdr.total_fail);
    printf("║    Fail-Solder       : %-5d             ║\n", hdr.fail_solder);
    printf("║    Fail-Placement    : %-5d             ║\n", hdr.fail_placement);
    printf("║    Fail-Continuity   : %-5d             ║\n", hdr.fail_continuity);
    printf("╚══════════════════════════════════════════╝\n");

    /* Write final snapshot */
    if (g_stats) {
        sem_wait(g_shm_sem);
        logger_write_snapshot(g_stats, &hdr);
        sem_post(g_shm_sem);
    }
}

/* ── Main ────────────────────────────────────────────────── */

int main(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║  PCB Quality Inspection Line Controller  ║\n");
    printf("║  PID: %-5d                              ║\n", getpid());
    printf("╚══════════════════════════════════════════╝\n\n");

    /* 1. Install signal handlers */
    if (install_signals() < 0) return EXIT_SIGNAL_ERROR;
    printf("[%s] Signal handlers installed\n", ROLE_CONTROLLER);

    /* 2. Initialize logger (creates log files, writes header) */
    if (logger_init(&g_log, ROLE_CONTROLLER) < 0) return EXIT_LOG_ERROR;
    printf("[%s] Logger initialized\n", ROLE_CONTROLLER);

    /* 3. Create IPC resources */
    if (create_fifos()          < 0) goto cleanup;
    if (create_shared_memory()  < 0) goto cleanup;
    if (create_semaphore()      < 0) goto cleanup;
    if (create_message_queue()  < 0) goto cleanup;

    /* 4. Spawn all child processes */
    printf("[%s] Spawning child processes...\n", ROLE_CONTROLLER);

    /* Spawn in order: stations first (they open FIFOs for read/write),
       then board_gen (opens FIFO_0 write end last to unblock stations),
       dashboard last */
    int spawn_order[] = { 1, 2, 3, 4, 0 }; /* station1,2,3 dashboard gen */

    for (int i = 0; i < NUM_CHILDREN; i++) {
        int idx = spawn_order[i];
        if (spawn_child(&g_children[idx]) < 0)
            goto shutdown;
        usleep(100000); /* 100ms stagger to allow FIFO opens to sequence */
    }

    logger_write_event(&g_log, ROLE_CONTROLLER,
                       "All child processes spawned — simulation running");

    /* 5. Start periodic alarm */
    alarm(SIM_ALARM_INTERVAL_SEC);
    printf("[%s] SIGALRM armed (%ds interval)\n",
           ROLE_CONTROLLER, SIM_ALARM_INTERVAL_SEC);

    /* 6. Main supervision loop */
    printf("[%s] Entering supervision loop\n", ROLE_CONTROLLER);

    while (!g_shutdown) {

        /* Handle SIGCHLD — reap any exited children */
        if (g_child_died) {
            g_child_died = 0;
            reap_children();
        }

        /* Handle SIGALRM — log throughput */
        if (g_alarm_tick) {
            g_alarm_tick = 0;
            log_throughput();
        }

        /* Handle SIGUSR1 — generate snapshot */
        if (g_snapshot) {
            g_snapshot = 0;
            printf("[%s] SIGUSR1: generating snapshot...\n", ROLE_CONTROLLER);
            logger_write_event(&g_log, ROLE_CONTROLLER,
                               "SIGUSR1 received — writing snapshot");

            if (g_stats) {
                sem_wait(g_shm_sem);
                log_header_t hdr;
                logger_read_header(&g_log, &hdr);
                logger_write_snapshot(g_stats, &hdr);
                sem_post(g_shm_sem);
            }
        }

        /* Drain message queue for defect alerts */
        drain_message_queue();

        /* Check if all children have exited naturally */
        if (all_children_done()) {
            printf("[%s] All children exited naturally — shutting down\n",
                   ROLE_CONTROLLER);
            break;
        }

        /* Sleep briefly to avoid busy-wait */
        usleep(100000);
    }

shutdown:
    if (g_shutdown) {
        printf("\n[%s] SIGINT received — initiating graceful shutdown\n",
               ROLE_CONTROLLER);
        logger_write_event(&g_log, ROLE_CONTROLLER,
                           "SIGINT received — graceful shutdown initiated");

        /* Cancel alarm */
        alarm(0);

        /* Forward SIGUSR2 to all children to stop them */
        signal_all_children(SIGUSR2);

        /* Wait 1s then SIGTERM any still-running children */
        sleep(1);
        signal_all_children(SIGTERM);
    }

    /* 7. Wait for all children — no zombies */
    wait_all_children();

    /* 8. Final drain of message queue */
    drain_message_queue();

    /* 9. Print and write final summary */
    print_summary();

    logger_write_event(&g_log, ROLE_CONTROLLER,
                       "Simulation complete — controller exiting");

cleanup:
    /* 10. Cleanup all IPC resources */
    cleanup_ipc();
    logger_close(&g_log);

    printf("[%s] All resources cleaned up. Goodbye.\n\n", ROLE_CONTROLLER);
    return EXIT_OK;
}
