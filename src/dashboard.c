/* =============================================================================
 * dashboard.c — Live Stats Dashboard Process
 * =============================================================================
 * Reads the stats_t block from shared memory (protected by semaphore)
 * and prints a live updating terminal display every SIM_DASHBOARD_REFRESH µs.
 *
 * IPC: Shared Memory (read), POSIX Semaphore (sync)
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/shm.h>
#include <errno.h>

#include "../include/board.h"
#include "../include/ipc_config.h"

static volatile sig_atomic_t g_stop = 0;
static void handle_stop(int sig) { (void)sig; g_stop = 1; }

/* ── Terminal helpers ────────────────────────────────────── */

#define CLEAR_SCREEN "\033[2J\033[H"
#define COLOR_GREEN  "\033[0;32m"
#define COLOR_RED    "\033[0;31m"
#define COLOR_YELLOW "\033[0;33m"
#define COLOR_CYAN   "\033[0;36m"
#define COLOR_BOLD   "\033[1m"
#define COLOR_RESET  "\033[0m"

static void print_bar(float value, float max_val, int width)
{
    int filled = (int)((value / max_val) * width);
    if (filled > width) filled = width;

    printf("[");
    for (int i = 0; i < width; i++)
        printf(i < filled ? "█" : "░");
    printf("]");
}

/* ── Dashboard Render ────────────────────────────────────── */

static void render(const stats_t *s)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(ts, sizeof(ts), "%H:%M:%S", t);

    printf(CLEAR_SCREEN);
    printf(COLOR_BOLD COLOR_CYAN);
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║     PCB QUALITY INSPECTION LINE  —  LIVE DASHBOARD  ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);
    printf("  Updated: %s\n\n", ts);

    /* Throughput */
    printf(COLOR_BOLD "  ── THROUGHPUT ─────────────────────────────────────\n"
           COLOR_RESET);
    printf("  Station 1 (Solder)    : %4d boards seen\n",
           s->boards_at_station[0]);
    printf("  Station 2 (Placement) : %4d boards seen\n",
           s->boards_at_station[1]);
    printf("  Station 3 (Continuity): %4d boards seen\n",
           s->boards_at_station[2]);
    printf("  Completed             : %4d boards  |  %.1f boards/min\n\n",
           s->boards_completed, s->throughput_per_min);

    /* Quality */
    printf(COLOR_BOLD "  ── QUALITY ──────────────────────────────────────────\n"
           COLOR_RESET);

    float dr = s->defect_rate_percent;
    const char *dr_color = (dr < 10.0f) ? COLOR_GREEN
                         : (dr < 20.0f) ? COLOR_YELLOW
                                        : COLOR_RED;

    printf("  Total Defects  : %s%d%s\n",
           COLOR_RED, s->total_defects, COLOR_RESET);
    printf("  Defect Rate    : %s%.2f%%%s  ",
           dr_color, dr, COLOR_RESET);
    print_bar(dr, 100.0f, 30);
    printf("\n\n");

    printf("  Defects per Station:\n");
    printf("    Stn1-Solder    : %d\n", s->defects_station[0]);
    printf("    Stn2-Placement : %d\n", s->defects_station[1]);
    printf("    Stn3-Continuity: %d\n\n", s->defects_station[2]);

    /* Averages */
    printf(COLOR_BOLD "  ── AVERAGES ─────────────────────────────────────────\n"
           COLOR_RESET);
    printf("  Avg Solder Score    : %.1f / 100  ", s->avg_solder_score);
    print_bar(s->avg_solder_score, 100.0f, 20);
    printf("\n");
    printf("  Avg Placement Error : %.2f mm\n",  s->avg_placement_offset_mm);
    printf("  Avg Continuity Res  : %.1f ohm\n", s->avg_continuity_resistance);

    printf("\n");
    printf(COLOR_BOLD "  ── TARGET ─────────────────────────────────────────\n"
           COLOR_RESET);
    printf("  Boards to inspect : %d\n", SIM_BOARD_COUNT);
    printf("  Progress          : ");
    print_bar((float)s->boards_completed, (float)SIM_BOARD_COUNT, 40);
    printf("  %d%%\n",
           (int)(100.0f * s->boards_completed / SIM_BOARD_COUNT));

    printf("\n  Press Ctrl+C to send SIGINT (graceful shutdown)\n");
    printf("  Send SIGUSR1 to trigger snapshot report\n");
    fflush(stdout);
}

/* ── Main ────────────────────────────────────────────────── */

int main(void)
{
    printf("[%s] Dashboard starting (PID=%d)\n", ROLE_DASHBOARD, getpid());

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Attach shared memory */
    key_t   shm_key = GET_SHM_KEY();
    int     shm_id  = -1;

    /* Wait for parent to create the shared memory segment */
    for (int retry = 0; retry < 20 && !g_stop; retry++) {
        shm_id = shmget(shm_key, sizeof(stats_t), SHM_PERMS);
        if (shm_id >= 0) break;
        printf("[%s] Waiting for shared memory...\n", ROLE_DASHBOARD);
        usleep(200000);
    }

    if (shm_id < 0) {
        SYS_ERR("shmget dashboard");
        return EXIT_SHM_ERROR;
    }

    stats_t *stats = (stats_t *)shmat(shm_id, NULL, SHM_RDONLY);
    if (stats == (void *)-1) {
        SYS_ERR("shmat dashboard");
        return EXIT_SHM_ERROR;
    }

    /* Open semaphore */
    sem_t *sem = sem_open(SEM_SHM_NAME, 0);
    if (sem == SEM_FAILED) {
        SYS_ERR("sem_open dashboard");
        return EXIT_SEM_ERROR;
    }

    printf("[%s] Shared memory attached — starting display loop\n",
           ROLE_DASHBOARD);

    /* Main display loop */
    while (!g_stop) {
        stats_t snapshot;

        /* Take a consistent snapshot under semaphore */
        sem_wait(sem);
        memcpy(&snapshot, stats, sizeof(stats_t));
        sem_post(sem);

        render(&snapshot);

        /* Exit when all boards are done */
        if (snapshot.boards_completed >= SIM_BOARD_COUNT) {
            printf("\n[%s] All boards completed — final stats above\n",
                   ROLE_DASHBOARD);
            sleep(2);
            break;
        }

        usleep(SIM_DASHBOARD_REFRESH);
    }

    shmdt(stats);
    sem_close(sem);

    printf("[%s] Clean exit\n", ROLE_DASHBOARD);
    return EXIT_OK;
}
