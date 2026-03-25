/* =============================================================================
 * station1.c — Station 1: Solder Quality Inspector
 * =============================================================================
 * Reads board_t structs from FIFO_0, inspects solder joints,
 * attaches result to board, writes to FIFO_1 for Station 2.
 *
 * Internal thread model:
 *   Thread 1 (reader)   — reads board_t from FIFO_0
 *   Thread 2 (inspector)— simulates solder joint scoring
 *   Thread 3 (writer)   — writes result to FIFO_1 + updates shared memory
 *
 * Sync: mutex + condition variable guards internal board queue
 * IPC : FIFO_0 (read), FIFO_1 (write), Shared Memory (stats update)
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
#include <signal.h>

#include "../include/board.h"
#include "../include/ipc_config.h"
#include "../include/logger.h"

/* ── Internal pipeline queue (between T1 and T2) ─────────── */
#define QUEUE_SIZE 8

typedef struct {
    board_t         boards[QUEUE_SIZE];
    solder_result_t results[QUEUE_SIZE]; /* filled by T2, passed to T3 */
    int             head, tail, count;
    int             done;               /* set when reader hits sentinel */
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} board_queue_t;

/* ── Shared between threads ──────────────────────────────── */
static board_queue_t  g_raw_q;    /* T1 → T2: boards to inspect      */
static board_queue_t  g_res_q;    /* T2 → T3: inspected boards+result */

static int      g_fifo0_fd  = -1;
static int      g_fifo1_fd  = -1;
static stats_t *g_stats     = NULL;
static sem_t   *g_shm_sem   = NULL;
static logger_ctx_t g_log;

static volatile sig_atomic_t g_stop = 0;

/* ── Queue helpers ───────────────────────────────────────── */

static void queue_init(board_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full,  NULL);
}

static void queue_destroy(board_queue_t *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static void queue_push_board(board_queue_t *q, const board_t *b)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == QUEUE_SIZE && !q->done)
        pthread_cond_wait(&q->not_full, &q->mutex);

    q->boards[q->tail] = *b;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static int queue_pop_board(board_queue_t *q, board_t *b)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->count == 0 && q->done) {
        pthread_mutex_unlock(&q->mutex);
        return -1; /* No more items */
    }

    *b = q->boards[q->head];
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

/* Push board + result together into result queue */
typedef struct { board_t b; solder_result_t r; } board_res_t;

static board_res_t g_res_items[QUEUE_SIZE];
static int         g_res_head = 0, g_res_tail = 0, g_res_count = 0;
static int         g_res_done = 0;
static pthread_mutex_t g_res_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_res_ne    = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  g_res_nf    = PTHREAD_COND_INITIALIZER;

static void res_push(const board_t *b, const solder_result_t *r)
{
    pthread_mutex_lock(&g_res_mutex);
    while (g_res_count == QUEUE_SIZE)
        pthread_cond_wait(&g_res_nf, &g_res_mutex);

    g_res_items[g_res_tail].b = *b;
    g_res_items[g_res_tail].r = *r;
    g_res_tail = (g_res_tail + 1) % QUEUE_SIZE;
    g_res_count++;

    pthread_cond_signal(&g_res_ne);
    pthread_mutex_unlock(&g_res_mutex);
}

static int res_pop(board_t *b, solder_result_t *r)
{
    pthread_mutex_lock(&g_res_mutex);
    while (g_res_count == 0 && !g_res_done)
        pthread_cond_wait(&g_res_ne, &g_res_mutex);

    if (g_res_count == 0 && g_res_done) {
        pthread_mutex_unlock(&g_res_mutex);
        return -1;
    }

    *b = g_res_items[g_res_head].b;
    *r = g_res_items[g_res_head].r;
    g_res_head = (g_res_head + 1) % QUEUE_SIZE;
    g_res_count--;

    pthread_cond_signal(&g_res_nf);
    pthread_mutex_unlock(&g_res_mutex);
    return 0;
}

/* ── Solder Inspection Logic ─────────────────────────────── */

static void inspect_solder(const board_t *b, solder_result_t *r,
                            unsigned int *seed)
{
    memset(r, 0, sizeof(*r));
    r->num_joints_checked = b->num_solder_joints;
    r->worst_joint_score  = 100;
    r->station_verdict    = VERDICT_PASS;

    for (int i = 0; i < b->num_solder_joints; i++) {
        int score;

        if (b->simulated_defect_flag && i == b->num_solder_joints / 2) {
            /* Plant a bad joint at the midpoint of defective boards */
            score = 30 + (rand_r(seed) % 25); /* score 30-54 = cold joint */
        } else {
            score = 70 + (rand_r(seed) % 31); /* score 70-100 = good */
        }

        r->joint_scores[i] = score;

        if (score < SOLDER_MIN_QUALITY) {
            r->num_defective_joints++;
        }

        if (score < r->worst_joint_score) {
            r->worst_joint_score = score;
            r->worst_joint_index = i;
        }
    }

    if (r->num_defective_joints > 0)
        r->station_verdict = VERDICT_FAIL_SOLDER;
}

/* ── Shared Memory Update ────────────────────────────────── */

static void update_shm(const solder_result_t *r)
{
    if (!g_stats || !g_shm_sem) return;

    sem_wait(g_shm_sem);

    g_stats->boards_at_station[0]++;
    if (r->station_verdict != VERDICT_PASS) {
        g_stats->defects_station[0]++;
        g_stats->total_defects++;
    }

    /* Rolling average solder score */
    int n = g_stats->boards_at_station[0];
    g_stats->avg_solder_score =
        ((g_stats->avg_solder_score * (n - 1)) + r->worst_joint_score) / n;

    g_stats->stats_updated_at = time(NULL);

    sem_post(g_shm_sem);
}

/* ── THREAD 1: FIFO Reader ───────────────────────────────── */

static void *thread_reader(void *arg)
{
    (void)arg;
    printf("[%s] T1-Reader started\n", ROLE_STATION_1);

    board_t board;
    while (!g_stop) {
        ssize_t rd = read(g_fifo0_fd, &board, sizeof(board_t));
        if (rd <= 0) break;

        /* Sentinel check */
        if (board.board_id == 0) {
            printf("[%s] T1-Reader: sentinel received — done\n", ROLE_STATION_1);
            break;
        }

        printf("[%s] T1 read  board %-10s\n", ROLE_STATION_1, board.board_name);
        queue_push_board(&g_raw_q, &board);
    }

    pthread_mutex_lock(&g_raw_q.mutex);
    g_raw_q.done = 1;
    pthread_cond_broadcast(&g_raw_q.not_empty);
    pthread_mutex_unlock(&g_raw_q.mutex);

    printf("[%s] T1-Reader exiting\n", ROLE_STATION_1);
    return NULL;
}

/* ── THREAD 2: Inspector ─────────────────────────────────── */

static void *thread_inspector(void *arg)
{
    (void)arg;
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    printf("[%s] T2-Inspector started\n", ROLE_STATION_1);

    board_t         board;
    solder_result_t result;

    while (1) {
        if (queue_pop_board(&g_raw_q, &board) < 0) break;

        printf("[%s] T2 inspecting board %-10s (%d joints)\n",
               ROLE_STATION_1, board.board_name, board.num_solder_joints);

        /* Simulate inspection time */
        usleep(SIM_STATION1_DELAY_US);

        inspect_solder(&board, &result, &seed);
        update_shm(&result);

        printf("[%s] T2 done  board %-10s | verdict=%s defects=%d\n",
               ROLE_STATION_1,
               board.board_name,
               VERDICT_STR(result.station_verdict),
               result.num_defective_joints);

        res_push(&board, &result);
    }

    pthread_mutex_lock(&g_res_mutex);
    g_res_done = 1;
    pthread_cond_broadcast(&g_res_ne);
    pthread_mutex_unlock(&g_res_mutex);

    printf("[%s] T2-Inspector exiting\n", ROLE_STATION_1);
    return NULL;
}

/* ── THREAD 3: FIFO Writer ───────────────────────────────── */

/* Packed struct written to FIFO_1: board + solder result */
typedef struct {
    board_t         board;
    solder_result_t solder;
} fifo1_packet_t;

static void *thread_writer(void *arg)
{
    (void)arg;
    printf("[%s] T3-Writer started\n", ROLE_STATION_1);

    board_t         board;
    solder_result_t result;

    while (1) {
        if (res_pop(&board, &result) < 0) break;

        fifo1_packet_t pkt;
        pkt.board  = board;
        pkt.solder = result;

        ssize_t written = write(g_fifo1_fd, &pkt, sizeof(fifo1_packet_t));
        if (written != (ssize_t)sizeof(fifo1_packet_t)) {
            SYS_ERR("write to FIFO_1");
            break;
        }

        printf("[%s] T3 forwarded board %-10s → Station 2\n",
               ROLE_STATION_1, board.board_name);
    }

    /* Write zero-sentinel to FIFO_1 */
    fifo1_packet_t sentinel;
    memset(&sentinel, 0, sizeof(fifo1_packet_t));
    write(g_fifo1_fd, &sentinel, sizeof(fifo1_packet_t));

    printf("[%s] T3-Writer exiting\n", ROLE_STATION_1);
    return NULL;
}

/* ── Signal Handler ──────────────────────────────────────── */

static void handle_sigusr2(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ── Main ────────────────────────────────────────────────── */

int main(void)
{
    printf("[%s] Process started (PID=%d)\n", ROLE_STATION_1, getpid());

    /* Install SIGUSR2 handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr2;
    sigaction(SIGUSR2, &sa, NULL);

    /* Attach shared memory */
    key_t shm_key = GET_SHM_KEY();
    int   shm_id  = shmget(shm_key, sizeof(stats_t), SHM_PERMS);
    if (shm_id < 0) { SYS_ERR("shmget station1"); return EXIT_SHM_ERROR; }

    g_stats = (stats_t *)shmat(shm_id, NULL, 0);
    if (g_stats == (void *)-1) { SYS_ERR("shmat station1"); return EXIT_SHM_ERROR; }

    /* Open named semaphore */
    g_shm_sem = sem_open(SEM_SHM_NAME, 0);
    if (g_shm_sem == SEM_FAILED) { SYS_ERR("sem_open station1"); return EXIT_SEM_ERROR; }

    /* Open FIFOs */
    printf("[%s] Opening FIFO_0 for reading...\n", ROLE_STATION_1);
    g_fifo0_fd = open(FIFO_0_PATH, O_RDONLY);
    if (g_fifo0_fd < 0) { SYS_ERR("open FIFO_0"); return EXIT_FIFO_ERROR; }

    printf("[%s] Opening FIFO_1 for writing...\n", ROLE_STATION_1);
    g_fifo1_fd = open(FIFO_1_PATH, O_WRONLY);
    if (g_fifo1_fd < 0) { SYS_ERR("open FIFO_1"); return EXIT_FIFO_ERROR; }

    /* Init queues */
    queue_init(&g_raw_q);
    queue_init(&g_res_q);

    /* Launch threads */
    pthread_t t1, t2, t3;
    if (pthread_create(&t1, NULL, thread_reader,    NULL) != 0) return EXIT_THREAD_ERROR;
    if (pthread_create(&t2, NULL, thread_inspector, NULL) != 0) return EXIT_THREAD_ERROR;
    if (pthread_create(&t3, NULL, thread_writer,    NULL) != 0) return EXIT_THREAD_ERROR;

    /* Wait for all threads to complete */
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    /* Cleanup */
    queue_destroy(&g_raw_q);
    queue_destroy(&g_res_q);
    close(g_fifo0_fd);
    close(g_fifo1_fd);
    shmdt(g_stats);
    sem_close(g_shm_sem);

    printf("[%s] Clean exit\n", ROLE_STATION_1);
    return EXIT_OK;
}
