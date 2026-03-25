/* =============================================================================
 * station2.c — Station 2: Component Placement Checker
 * =============================================================================
 * Reads fifo1_packet_t (board + solder result) from FIFO_1,
 * checks component placement offsets,
 * writes fifo2_packet_t (board + solder + placement result) to FIFO_2.
 *
 * Internal thread model:
 *   Thread 1 (reader)   — reads fifo1_packet_t from FIFO_1
 *   Thread 2 (inspector)— simulates X/Y placement offset checks
 *   Thread 3 (writer)   — writes result to FIFO_2 + updates shared memory
 *
 * IPC: FIFO_1 (read), FIFO_2 (write), Shared Memory (stats update)
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

/* ── Packet types ────────────────────────────────────────── */

/* What we READ from FIFO_1 */
typedef struct {
    board_t         board;
    solder_result_t solder;
} fifo1_packet_t;

/* What we WRITE to FIFO_2 */
typedef struct {
    board_t            board;
    solder_result_t    solder;
    placement_result_t placement;
} fifo2_packet_t;

/* ── Internal queue between threads ─────────────────────── */
#define QUEUE_SIZE 8

typedef struct {
    fifo1_packet_t items[QUEUE_SIZE];
    int  head, tail, count, done;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} input_queue_t;

typedef struct {
    fifo2_packet_t items[QUEUE_SIZE];
    int  head, tail, count, done;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} output_queue_t;

static input_queue_t  g_in_q;
static output_queue_t g_out_q;

/* ── Globals ─────────────────────────────────────────────── */
static int      g_fifo1_fd = -1;
static int      g_fifo2_fd = -1;
static stats_t *g_stats    = NULL;
static sem_t   *g_shm_sem  = NULL;

static volatile sig_atomic_t g_stop = 0;

/* ── Queue helpers ───────────────────────────────────────── */

static void inq_init(input_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full,  NULL);
}
static void outq_init(output_queue_t *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full,  NULL);
}
static void inq_destroy(input_queue_t *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}
static void outq_destroy(output_queue_t *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

static void inq_push(input_queue_t *q, const fifo1_packet_t *p) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == QUEUE_SIZE && !q->done)
        pthread_cond_wait(&q->not_full, &q->mutex);
    q->items[q->tail] = *p;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static int inq_pop(input_queue_t *q, fifo1_packet_t *p) {
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

static void outq_push(output_queue_t *q, const fifo2_packet_t *p) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == QUEUE_SIZE && !q->done)
        pthread_cond_wait(&q->not_full, &q->mutex);
    q->items[q->tail] = *p;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static int outq_pop(output_queue_t *q, fifo2_packet_t *p) {
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

/* ── Placement Inspection Logic ──────────────────────────── */

static void inspect_placement(const board_t *b, placement_result_t *r,
                               unsigned int *seed)
{
    memset(r, 0, sizeof(*r));
    r->num_components_checked = b->num_components;
    r->station_verdict        = VERDICT_PASS;

    for (int i = 0; i < b->num_components; i++) {
        float dx, dy;

        if (b->simulated_defect_flag && i == 2) {
            /* Plant a misplacement on component index 2 */
            dx = 2.0f + ((float)(rand_r(seed) % 100) / 50.0f);
            dy = 1.5f + ((float)(rand_r(seed) % 100) / 50.0f);
        } else {
            dx = ((float)(rand_r(seed) % 200)) / 100.0f; /* 0.0–2.0 mm */
            dy = ((float)(rand_r(seed) % 200)) / 100.0f;
        }

        r->x_offset[i] = dx;
        r->y_offset[i] = dy;

        float offset = sqrtf(dx * dx + dy * dy);
        if (offset > PLACEMENT_MAX_OFFSET) {
            r->num_misplaced++;
        }

        if (offset > r->worst_offset_mm) {
            r->worst_offset_mm       = offset;
            r->worst_component_index = i;
        }
    }

    if (r->num_misplaced > 0)
        r->station_verdict = VERDICT_FAIL_PLACE;
}

/* ── Shared Memory Update ────────────────────────────────── */

static void update_shm(const placement_result_t *r)
{
    if (!g_stats || !g_shm_sem) return;

    sem_wait(g_shm_sem);

    g_stats->boards_at_station[1]++;
    if (r->station_verdict != VERDICT_PASS) {
        g_stats->defects_station[1]++;
        g_stats->total_defects++;
    }

    int n = g_stats->boards_at_station[1];
    g_stats->avg_placement_offset_mm =
        ((g_stats->avg_placement_offset_mm * (n - 1)) + r->worst_offset_mm) / n;

    g_stats->stats_updated_at = time(NULL);
    sem_post(g_shm_sem);
}

/* ── Signal Handler ──────────────────────────────────────── */

static void handle_sigusr2(int sig) { (void)sig; g_stop = 1; }

/* ── Thread 1: Reader ────────────────────────────────────── */

static void *thread_reader(void *arg)
{
    (void)arg;
    printf("[%s] T1-Reader started\n", ROLE_STATION_2);

    fifo1_packet_t pkt;
    while (!g_stop) {
        ssize_t rd = read(g_fifo1_fd, &pkt, sizeof(fifo1_packet_t));
        if (rd <= 0) break;

        if (pkt.board.board_id == 0) {
            printf("[%s] T1-Reader: sentinel — done\n", ROLE_STATION_2);
            break;
        }

        printf("[%s] T1 read  board %-10s\n", ROLE_STATION_2, pkt.board.board_name);
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
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid() ^ 0xAB;
    printf("[%s] T2-Inspector started\n", ROLE_STATION_2);

    fifo1_packet_t  in_pkt;
    placement_result_t result;

    while (1) {
        if (inq_pop(&g_in_q, &in_pkt) < 0) break;

        printf("[%s] T2 inspecting board %-10s (%d components)\n",
               ROLE_STATION_2, in_pkt.board.board_name,
               in_pkt.board.num_components);

        usleep(SIM_STATION2_DELAY_US);

        inspect_placement(&in_pkt.board, &result, &seed);
        update_shm(&result);

        printf("[%s] T2 done  board %-10s | verdict=%s misplaced=%d\n",
               ROLE_STATION_2,
               in_pkt.board.board_name,
               VERDICT_STR(result.station_verdict),
               result.num_misplaced);

        fifo2_packet_t out_pkt;
        out_pkt.board     = in_pkt.board;
        out_pkt.solder    = in_pkt.solder;
        out_pkt.placement = result;

        outq_push(&g_out_q, &out_pkt);
    }

    pthread_mutex_lock(&g_out_q.mutex);
    g_out_q.done = 1;
    pthread_cond_broadcast(&g_out_q.not_empty);
    pthread_mutex_unlock(&g_out_q.mutex);

    return NULL;
}

/* ── Thread 3: Writer ────────────────────────────────────── */

static void *thread_writer(void *arg)
{
    (void)arg;
    printf("[%s] T3-Writer started\n", ROLE_STATION_2);

    fifo2_packet_t pkt;
    while (1) {
        if (outq_pop(&g_out_q, &pkt) < 0) break;

        ssize_t written = write(g_fifo2_fd, &pkt, sizeof(fifo2_packet_t));
        if (written != (ssize_t)sizeof(fifo2_packet_t)) {
            SYS_ERR("write to FIFO_2");
            break;
        }

        printf("[%s] T3 forwarded board %-10s → Station 3\n",
               ROLE_STATION_2, pkt.board.board_name);
    }

    /* Write zero-sentinel to FIFO_2 */
    fifo2_packet_t sentinel;
    memset(&sentinel, 0, sizeof(fifo2_packet_t));
    write(g_fifo2_fd, &sentinel, sizeof(fifo2_packet_t));

    return NULL;
}

/* ── Main ────────────────────────────────────────────────── */

int main(void)
{
    printf("[%s] Process started (PID=%d)\n", ROLE_STATION_2, getpid());

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr2;
    sigaction(SIGUSR2, &sa, NULL);

    /* Shared memory */
    key_t shm_key = GET_SHM_KEY();
    int   shm_id  = shmget(shm_key, sizeof(stats_t), SHM_PERMS);
    if (shm_id < 0) { SYS_ERR("shmget station2"); return EXIT_SHM_ERROR; }

    g_stats = (stats_t *)shmat(shm_id, NULL, 0);
    if (g_stats == (void *)-1) { SYS_ERR("shmat station2"); return EXIT_SHM_ERROR; }

    g_shm_sem = sem_open(SEM_SHM_NAME, 0);
    if (g_shm_sem == SEM_FAILED) { SYS_ERR("sem_open station2"); return EXIT_SEM_ERROR; }

    /* FIFOs */
    printf("[%s] Opening FIFO_1 for reading...\n", ROLE_STATION_2);
    g_fifo1_fd = open(FIFO_1_PATH, O_RDONLY);
    if (g_fifo1_fd < 0) { SYS_ERR("open FIFO_1"); return EXIT_FIFO_ERROR; }

    printf("[%s] Opening FIFO_2 for writing...\n", ROLE_STATION_2);
    g_fifo2_fd = open(FIFO_2_PATH, O_WRONLY);
    if (g_fifo2_fd < 0) { SYS_ERR("open FIFO_2"); return EXIT_FIFO_ERROR; }

    inq_init(&g_in_q);
    outq_init(&g_out_q);

    pthread_t t1, t2, t3;
    pthread_create(&t1, NULL, thread_reader,    NULL);
    pthread_create(&t2, NULL, thread_inspector, NULL);
    pthread_create(&t3, NULL, thread_writer,    NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);

    inq_destroy(&g_in_q);
    outq_destroy(&g_out_q);
    close(g_fifo1_fd);
    close(g_fifo2_fd);
    shmdt(g_stats);
    sem_close(g_shm_sem);

    printf("[%s] Clean exit\n", ROLE_STATION_2);
    return EXIT_OK;
}
