/* =============================================================================
 * station3.c — Station 3: Electrical Continuity Tester
 * =============================================================================
 * Reads fifo2_packet_t from FIFO_2, tests electrical nets for
 * opens and shorts, determines final board verdict,
 * writes board_record_t to inspection log, and sends alert_msg_t
 * to the message queue if any defect is found.
 *
 * Internal thread model:
 *   Thread 1 (reader)   — reads fifo2_packet_t from FIFO_2
 *   Thread 2 (inspector)— simulates resistance measurements
 *   Thread 3 (writer)   — writes log record + sends msg queue alerts
 *                         + updates shared memory
 *
 * IPC: FIFO_2 (read), Message Queue (write alerts), Shared Memory (stats)
 * File I/O: logger_append_record() → inspection_log.bin
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <signal.h>

#include "../include/board.h"
#include "../include/ipc_config.h"
#include "../include/logger.h"

/* ── Packet type from FIFO_2 ─────────────────────────────── */
typedef struct {
    board_t            board;
    solder_result_t    solder;
    placement_result_t placement;
} fifo2_packet_t;

/* ── Internal queue ──────────────────────────────────────── */
#define QUEUE_SIZE 8

typedef struct {
    fifo2_packet_t items[QUEUE_SIZE];
    int  head, tail, count, done;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} in_queue_t;

typedef struct {
    board_record_t items[QUEUE_SIZE];
    int  head, tail, count, done;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} rec_queue_t;

static in_queue_t  g_in_q;
static rec_queue_t g_rec_q;

/* ── Globals ─────────────────────────────────────────────── */
static int      g_fifo2_fd = -1;
static int      g_msgq_id  = -1;
static stats_t *g_stats    = NULL;
static sem_t   *g_shm_sem  = NULL;
static logger_ctx_t g_log;

static volatile sig_atomic_t g_stop = 0;

/* ── Queue helpers ───────────────────────────────────────── */

static void inq_init(in_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full,  NULL);
}
static void recq_init(rec_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full,  NULL);
}
static void inq_destroy(in_queue_t *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}
static void recq_destroy(rec_queue_t *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static void inq_push(in_queue_t *q, const fifo2_packet_t *p) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == QUEUE_SIZE && !q->done)
        pthread_cond_wait(&q->not_full, &q->mutex);
    q->items[q->tail] = *p;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static int inq_pop(in_queue_t *q, fifo2_packet_t *p) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->mutex);
    if (q->count == 0 && q->done) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    *p = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static void recq_push(rec_queue_t *q, const board_record_t *r) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == QUEUE_SIZE && !q->done)
        pthread_cond_wait(&q->not_full, &q->mutex);
    q->items[q->tail] = *r;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static int recq_pop(rec_queue_t *q, board_record_t *r) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->mutex);
    if (q->count == 0 && q->done) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    *r = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

/* ── Continuity Inspection Logic ─────────────────────────── */

static void inspect_continuity(const board_t *b, continuity_result_t *r,
                                unsigned int *seed)
{
    memset(r, 0, sizeof(*r));
    r->num_nets_checked  = b->num_nets;
    r->worst_resistance  = 0.0f;
    r->station_verdict   = VERDICT_PASS;

    for (int i = 0; i < b->num_nets; i++) {
        float res;

        if (b->simulated_defect_flag && i == 3) {
            /* Plant an open circuit on net 3 */
            res = 10000.0f + (float)(rand_r(seed) % 5000);
        } else if (b->simulated_defect_flag && i == 5) {
            /* Plant a short circuit on net 5 */
            res = 0.0f + ((float)(rand_r(seed) % 10)) / 100.0f;
        } else {
            /* Normal resistance: 1–500 ohms */
            res = 1.0f + (float)(rand_r(seed) % 500);
        }

        r->resistance[i] = res;

        if (res > CONTINUITY_MAX_RES) r->num_opens++;
        if (res < CONTINUITY_MIN_RES) r->num_shorts++;

        if (res > r->worst_resistance) {
            r->worst_resistance = res;
            r->worst_net_index  = i;
        }
    }

    if (r->num_opens > 0 || r->num_shorts > 0)
        r->station_verdict = VERDICT_FAIL_CONT;
}

/* ── Determine Final Verdict ─────────────────────────────── */

static uint8_t final_verdict(const solder_result_t    *s,
                              const placement_result_t *p,
                              const continuity_result_t *c)
{
    int fails = (s->station_verdict != VERDICT_PASS ? 1 : 0)
              + (p->station_verdict != VERDICT_PASS ? 1 : 0)
              + (c->station_verdict != VERDICT_PASS ? 1 : 0);

    if (fails == 0)                        return VERDICT_PASS;
    if (fails > 1)                         return VERDICT_FAIL_MULTI;
    if (s->station_verdict != VERDICT_PASS) return VERDICT_FAIL_SOLDER;
    if (p->station_verdict != VERDICT_PASS) return VERDICT_FAIL_PLACE;
    return VERDICT_FAIL_CONT;
}

/* ── Shared Memory Update ────────────────────────────────── */

/* Track run start time for throughput calculation */
static time_t g_run_start = 0;

static void update_shm(const continuity_result_t *r, int completed)
{
    if (!g_stats || !g_shm_sem) return;

    sem_wait(g_shm_sem);

    g_stats->boards_at_station[2]++;
    if (r->station_verdict != VERDICT_PASS) {
        g_stats->defects_station[2]++;
        g_stats->total_defects++;
    }

    if (completed) {
        g_stats->boards_completed++;
        g_stats->last_board_time = time(NULL);

        /* Recalculate defect rate */
        if (g_stats->boards_completed > 0) {
            g_stats->defect_rate_percent =
                (100.0f * g_stats->total_defects) /
                (float)g_stats->boards_completed;
        }

        /* Calculate throughput: boards per minute since first board */
        if (g_run_start == 0)
            g_run_start = time(NULL);

        double elapsed_sec = difftime(time(NULL), g_run_start);
        if (elapsed_sec > 0.0) {
            g_stats->throughput_per_min =
                (float)(g_stats->boards_completed / (elapsed_sec / 60.0));
        }
    }

    int n = g_stats->boards_at_station[2];
    g_stats->avg_continuity_resistance =
        ((g_stats->avg_continuity_resistance * (n - 1)) +
         r->worst_resistance) / n;

    g_stats->stats_updated_at = time(NULL);
    sem_post(g_shm_sem);
}

/* ── Signal Handler ──────────────────────────────────────── */
static void handle_sigusr2(int sig) { (void)sig; g_stop = 1; }

/* ── Thread 1: Reader ────────────────────────────────────── */

static void *thread_reader(void *arg)
{
    (void)arg;
    printf("[%s] T1-Reader started\n", ROLE_STATION_3);

    fifo2_packet_t pkt;
    while (!g_stop) {
        ssize_t rd = read(g_fifo2_fd, &pkt, sizeof(fifo2_packet_t));
        if (rd <= 0) break;

        if (pkt.board.board_id == 0) {
            printf("[%s] T1-Reader: sentinel — done\n", ROLE_STATION_3);
            break;
        }

        printf("[%s] T1 read  board %-10s\n", ROLE_STATION_3, pkt.board.board_name);
        inq_push(&g_in_q, &pkt);
    }

    pthread_mutex_lock(&g_in_q.mutex);
    g_in_q.done = 1;
    pthread_cond_broadcast(&g_in_q.not_empty);
    pthread_mutex_unlock(&g_in_q.mutex);

    return NULL;
}

/* ── Thread 2: Inspector ─────────────────────────────────── */

static void *thread_inspector(void *arg)
{
    (void)arg;
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid() ^ 0xCD;
    printf("[%s] T2-Inspector started\n", ROLE_STATION_3);

    fifo2_packet_t     in_pkt;
    continuity_result_t result;

    while (1) {
        if (inq_pop(&g_in_q, &in_pkt) < 0) break;

        printf("[%s] T2 testing  board %-10s (%d nets)\n",
               ROLE_STATION_3, in_pkt.board.board_name,
               in_pkt.board.num_nets);

        usleep(SIM_STATION3_DELAY_US);

        inspect_continuity(&in_pkt.board, &result, &seed);
        update_shm(&result, 1);

        /* Build the full board record */
        board_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.board       = in_pkt.board;
        rec.solder      = in_pkt.solder;
        rec.placement   = in_pkt.placement;
        rec.continuity  = result;
        rec.final_verdict = final_verdict(&in_pkt.solder,
                                           &in_pkt.placement,
                                           &result);
        rec.exit_time     = time(NULL);
        rec.total_time_sec = difftime(rec.exit_time,
                                       in_pkt.board.entry_time);

        printf("[%s] T2 done   board %-10s | FINAL=%s opens=%d shorts=%d\n",
               ROLE_STATION_3,
               in_pkt.board.board_name,
               VERDICT_STR(rec.final_verdict),
               result.num_opens,
               result.num_shorts);

        recq_push(&g_rec_q, &rec);
    }

    pthread_mutex_lock(&g_rec_q.mutex);
    g_rec_q.done = 1;
    pthread_cond_broadcast(&g_rec_q.not_empty);
    pthread_mutex_unlock(&g_rec_q.mutex);

    return NULL;
}

/* ── Thread 3: Writer (log + msg queue) ─────────────────── */

static void *thread_writer(void *arg)
{
    (void)arg;
    printf("[%s] T3-Writer started\n", ROLE_STATION_3);

    board_record_t rec;
    while (1) {
        if (recq_pop(&g_rec_q, &rec) < 0) break;

        /* Write to binary inspection log */
        if (logger_append_record(&g_log, &rec) < 0) {
            fprintf(stderr, "[%s] Failed to write log record\n", ROLE_STATION_3);
        }

        /* Write to text alerts log and message queue on failure */
        if (rec.final_verdict != VERDICT_PASS) {

            alert_msg_t alert;
            memset(&alert, 0, sizeof(alert));
            alert.msg_type      = MSG_TYPE_ALERT;
            alert.board_id      = rec.board.board_id;
            strncpy(alert.board_name, rec.board.board_name, BOARD_ID_LEN - 1);
            alert.alert_station = 3;
            alert.defect_type   = rec.final_verdict;
            alert.alert_time    = time(NULL);

            snprintf(alert.description, sizeof(alert.description),
                     "Verdict=%s | Solder-fails=%d | Misplaced=%d | "
                     "Opens=%d Shorts=%d",
                     VERDICT_STR(rec.final_verdict),
                     rec.solder.num_defective_joints,
                     rec.placement.num_misplaced,
                     rec.continuity.num_opens,
                     rec.continuity.num_shorts);

            /* Write to alerts.log */
            logger_write_alert(&g_log, &alert);

            /* Send to message queue → Line Controller */
            if (g_msgq_id >= 0) {
                if (msgsnd(g_msgq_id, &alert, sizeof(alert_msg_t) - sizeof(long), 0) < 0) {
                    SYS_ERR("msgsnd alert");
                }
            }

            printf("[%s] T3 DEFECT alert sent for board %-10s\n",
                   ROLE_STATION_3, rec.board.board_name);
        }
    }

    /* Send MSG_TYPE_DONE sentinel to controller */
    if (g_msgq_id >= 0) {
        alert_msg_t done_msg;
        memset(&done_msg, 0, sizeof(done_msg));
        done_msg.msg_type = MSG_TYPE_DONE;
        msgsnd(g_msgq_id, &done_msg, sizeof(alert_msg_t) - sizeof(long), 0);
    }

    printf("[%s] T3-Writer exiting\n", ROLE_STATION_3);
    return NULL;
}

/* ── Main ────────────────────────────────────────────────── */

int main(void)
{
    printf("[%s] Process started (PID=%d)\n", ROLE_STATION_3, getpid());

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr2;
    sigaction(SIGUSR2, &sa, NULL);

    /* Open logger in append mode (parent created the file) */
    if (logger_open_append(&g_log, ROLE_STATION_3) < 0)
        return EXIT_LOG_ERROR;

    /* Shared memory */
    key_t shm_key = GET_SHM_KEY();
    int   shm_id  = shmget(shm_key, sizeof(stats_t), SHM_PERMS);
    if (shm_id < 0) { SYS_ERR("shmget station3"); return EXIT_SHM_ERROR; }

    g_stats = (stats_t *)shmat(shm_id, NULL, 0);
    if (g_stats == (void *)-1) { SYS_ERR("shmat station3"); return EXIT_SHM_ERROR; }

    /* Record run start time for throughput calculation */
    g_run_start = time(NULL);

    g_shm_sem = sem_open(SEM_SHM_NAME, 0);
    if (g_shm_sem == SEM_FAILED) { SYS_ERR("sem_open station3"); return EXIT_SEM_ERROR; }

    /* Message queue */
    key_t msg_key = GET_MSG_KEY();
    g_msgq_id = msgget(msg_key, MSG_PERMS);
    if (g_msgq_id < 0) { SYS_ERR("msgget station3"); return EXIT_MSG_ERROR; }

    /* FIFO_2 */
    printf("[%s] Opening FIFO_2 for reading...\n", ROLE_STATION_3);
    g_fifo2_fd = open(FIFO_2_PATH, O_RDONLY);
    if (g_fifo2_fd < 0) { SYS_ERR("open FIFO_2"); return EXIT_FIFO_ERROR; }

    inq_init(&g_in_q);
    recq_init(&g_rec_q);

    pthread_t t1, t2, t3;
    pthread_create(&t1, NULL, thread_reader,    NULL);
    pthread_create(&t2, NULL, thread_inspector, NULL);
    pthread_create(&t3, NULL, thread_writer,    NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    inq_destroy(&g_in_q);
    recq_destroy(&g_rec_q);
    close(g_fifo2_fd);
    shmdt(g_stats);
    sem_close(g_shm_sem);
    logger_close(&g_log);

    printf("[%s] Clean exit\n", ROLE_STATION_3);
    return EXIT_OK;
}
