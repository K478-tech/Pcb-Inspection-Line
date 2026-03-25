/* =============================================================================
 * board_gen.c — PCB Board Generator Process
 * =============================================================================
 * Simulates boards arriving on the inspection line.
 * Generates SIM_BOARD_COUNT boards with randomized properties,
 * writes each board_t struct into FIFO_0 for Station 1 to pick up.
 *
 * System calls used: open (FIFO), write, close, usleep
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

#include "../include/board.h"
#include "../include/ipc_config.h"

/* ── Globals ─────────────────────────────────────────────── */
static volatile sig_atomic_t g_stop = 0;

static void handle_sigusr2(int sig)
{
    (void)sig;
    g_stop = 1;
    printf("[%s] SIGUSR2 received — stopping board generation\n",
           ROLE_BOARD_GEN);
}

/* ── Board Simulation ────────────────────────────────────── */

static void generate_board(board_t *b, int id, unsigned int *seed)
{
    memset(b, 0, sizeof(board_t));

    b->board_id = id;
    snprintf(b->board_name, BOARD_ID_LEN, "PCB-%05d", id);

    /* Randomize board properties within realistic ranges */
    b->num_solder_joints = 16 + (rand_r(seed) % (MAX_SOLDER_JOINTS - 16 + 1));
    b->num_components    =  8 + (rand_r(seed) % (MAX_COMPONENTS   -  8 + 1));
    b->num_nets          = 12 + (rand_r(seed) % (MAX_NETS          - 12 + 1));
    b->entry_time        = time(NULL);

    /* Plant a simulated defect in 1 of every SIM_DEFECT_RATE_N boards */
    b->simulated_defect_flag = ((id % SIM_DEFECT_RATE_N) == 0) ? 1 : 0;
}

/* ── Main ────────────────────────────────────────────────── */

int main(void)
{
    printf("[%s] Starting — will generate %d boards\n",
           ROLE_BOARD_GEN, SIM_BOARD_COUNT);

    /* Install signal handler for SIGUSR2 (pause/stop command) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigusr2;
    sigaction(SIGUSR2, &sa, NULL);

    /* Open FIFO_0 for writing — blocks until Station 1 opens read end */
    printf("[%s] Opening FIFO_0 (%s) for writing...\n",
           ROLE_BOARD_GEN, FIFO_0_PATH);

    int fifo_fd = open(FIFO_0_PATH, O_WRONLY);
    if (fifo_fd < 0) {
        SYS_ERR("open FIFO_0 for writing");
        return EXIT_FIFO_ERROR;
    }

    printf("[%s] FIFO_0 open — beginning board generation\n", ROLE_BOARD_GEN);

    /* Seeded random generator for reproducibility */
    unsigned int seed = (SIM_RANDOM_SEED != 0)
                        ? (unsigned int)SIM_RANDOM_SEED
                        : (unsigned int)time(NULL);

    int boards_sent = 0;

    for (int i = 1; i <= SIM_BOARD_COUNT && !g_stop; i++) {

        board_t board;
        generate_board(&board, i, &seed);

        printf("[%s] Board %-10s | joints=%-2d comps=%-2d nets=%-2d %s\n",
               ROLE_BOARD_GEN,
               board.board_name,
               board.num_solder_joints,
               board.num_components,
               board.num_nets,
               board.simulated_defect_flag ? "[DEFECT PLANTED]" : "");

        /* Write board_t struct into FIFO_0 */
        ssize_t written = write(fifo_fd, &board, sizeof(board_t));
        if (written != (ssize_t)sizeof(board_t)) {
            if (errno == EPIPE) {
                printf("[%s] FIFO_0 broken pipe — Station 1 closed\n",
                       ROLE_BOARD_GEN);
                break;
            }
            SYS_ERR("write board to FIFO_0");
            break;
        }

        boards_sent++;

        /* Simulate realistic board arrival interval */
        usleep(SIM_BOARD_INTERVAL_US);
    }

    /* Send a zeroed sentinel board (board_id = 0) to signal end of input */
    board_t sentinel;
    memset(&sentinel, 0, sizeof(board_t));
    sentinel.board_id = 0;   /* Sentinel marker */
    write(fifo_fd, &sentinel, sizeof(board_t));

    printf("[%s] Done — %d boards sent to line. Sentinel written.\n",
           ROLE_BOARD_GEN, boards_sent);

    close(fifo_fd);
    return EXIT_OK;
}
