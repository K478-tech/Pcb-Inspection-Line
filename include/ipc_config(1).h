/* =============================================================================
 * ipc_config.h — IPC Configuration for PCB Quality Inspection Line
 * =============================================================================
 * Central configuration file for ALL inter-process communication resources.
 * Every process/module that needs IPC must include this file.
 *
 * Resources defined here:
 *   - Named FIFO paths        (board data pipeline between stations)
 *   - Shared memory key       (live stats block for dashboard)
 *   - POSIX semaphore name    (protects shared memory writes)
 *   - Message queue key       (defect alerts → line controller)
 *   - Simulation parameters   (timing, board count, anomaly rate)
 *   - Process exit codes      (standardized child exit values)
 * =============================================================================
 */

#ifndef IPC_CONFIG_H
#define IPC_CONFIG_H

#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>

/* ─────────────────────────────────────────────────────────
 * NAMED FIFO PATHS (Named Pipes)
 * Used to pass board_t structs between stations in sequence:
 *   Board Generator → FIFO_0 → Station 1
 *   Station 1       → FIFO_1 → Station 2
 *   Station 2       → FIFO_2 → Station 3
 * ───────────────────────────────────────────────────────── */
#define FIFO_0_PATH     "/tmp/pcb_fifo_0"   /* Board Gen   → Station 1      */
#define FIFO_1_PATH     "/tmp/pcb_fifo_1"   /* Station 1   → Station 2      */
#define FIFO_2_PATH     "/tmp/pcb_fifo_2"   /* Station 2   → Station 3      */

#define FIFO_PERMS      0666                /* Read/write for all processes  */

/* Total number of FIFOs — used in cleanup loops */
#define NUM_FIFOS       3

/* Array of all FIFO paths — used in cleanup */
#define FIFO_PATHS      { FIFO_0_PATH, FIFO_1_PATH, FIFO_2_PATH }


/* ─────────────────────────────────────────────────────────
 * SHARED MEMORY — Live Stats Block
 * Holds a stats_t struct written by all station processes
 * and read continuously by the Dashboard process.
 *
 * Using System V shmget/shmat (key-based) for compatibility.
 * Key is generated from a known file + project ID.
 * ───────────────────────────────────────────────────────── */
#define SHM_KEY_PATH    "/tmp"              /* File used for ftok()          */
#define SHM_KEY_ID      'P'                 /* Project ID for ftok()         */

/* Permissions for shared memory segment */
#define SHM_PERMS       0666

/* Helper macro to generate the shared memory key at runtime */
#define GET_SHM_KEY()   ftok(SHM_KEY_PATH, SHM_KEY_ID)


/* ─────────────────────────────────────────────────────────
 * POSIX SEMAPHORE — Shared Memory Protection
 * A single named semaphore guards the stats_t shared memory.
 * Any process writing to shared memory must:
 *   1. sem_wait()  — acquire lock
 *   2. write data  — update stats_t fields
 *   3. sem_post()  — release lock
 * Dashboard reads also use sem_wait/sem_post for consistency.
 * ───────────────────────────────────────────────────────── */
#define SEM_SHM_NAME    "/pcb_shm_sem"      /* Named semaphore for shm guard */
#define SEM_INITIAL_VAL 1                   /* Binary semaphore (mutex-like) */


/* ─────────────────────────────────────────────────────────
 * MESSAGE QUEUE — Defect Alert Channel
 * Station 3 sends alert_msg_t packets to the Line Controller
 * whenever a board fails any inspection stage.
 *
 * Using System V msgget (key-based).
 * ───────────────────────────────────────────────────────── */
#define MSG_KEY_PATH    "/tmp"              /* File used for ftok()          */
#define MSG_KEY_ID      'Q'                 /* Project ID for ftok()         */

/* Message queue creation flags */
#define MSG_PERMS       0666

/* Message type used in alert_msg_t.msg_type */
#define MSG_TYPE_ALERT  1                   /* All defect alerts use type 1  */
#define MSG_TYPE_DONE   2                   /* Sentinel: simulation complete */

/* Helper macro to generate the message queue key at runtime */
#define GET_MSG_KEY()   ftok(MSG_KEY_PATH, MSG_KEY_ID)

/* Max messages allowed in queue before blocking */
#define MSG_MAX_QUEUE   64


/* ─────────────────────────────────────────────────────────
 * SIMULATION PARAMETERS
 * Controls how the simulation behaves — adjust for testing
 * ───────────────────────────────────────────────────────── */

/* Number of boards to generate per simulation run */
#define SIM_BOARD_COUNT         50

/* Delay between board generation (microseconds) */
#define SIM_BOARD_INTERVAL_US   200000      /* 200ms between each board      */

/* Inspection time simulation per station (microseconds) */
#define SIM_STATION1_DELAY_US   150000      /* Solder check: 150ms           */
#define SIM_STATION2_DELAY_US   120000      /* Placement check: 120ms        */
#define SIM_STATION3_DELAY_US   180000      /* Continuity test: 180ms        */

/* Defect injection rate (1 in N boards will have a planted defect) */
#define SIM_DEFECT_RATE_N       7           /* ~14% defect rate              */

/* Random seed for reproducible runs (0 = use time-based seed) */
#define SIM_RANDOM_SEED         42

/* SIGALRM interval — how often throughput is logged (seconds) */
#define SIM_ALARM_INTERVAL_SEC  5

/* Dashboard refresh rate (microseconds) */
#define SIM_DASHBOARD_REFRESH   500000      /* Refresh every 500ms           */


/* ─────────────────────────────────────────────────────────
 * LOG FILE PATHS
 * ───────────────────────────────────────────────────────── */
#define LOG_DIR             "logs"
#define LOG_BINARY_PATH     "logs/inspection_log.bin"
#define LOG_ALERTS_PATH     "logs/alerts.log"
#define LOG_SNAPSHOT_PATH   "logs/snapshot_report.txt"

/* Magic number for binary log file header integrity check */
#define LOG_MAGIC           0xPCB1234        /* Written to log_header_t.magic */

/* ─────────────────────────────────────────────────────────
 * PROCESS EXIT CODES
 * Standardized exit codes for all child processes.
 * Parent uses these with waitpid() to detect failures.
 * ───────────────────────────────────────────────────────── */
#define EXIT_OK                 0   /* Normal clean exit                     */
#define EXIT_FIFO_ERROR         10  /* Failed to open or read/write FIFO     */
#define EXIT_SHM_ERROR          11  /* Failed to attach shared memory        */
#define EXIT_SEM_ERROR          12  /* Failed to open semaphore              */
#define EXIT_MSG_ERROR          13  /* Failed to access message queue        */
#define EXIT_LOG_ERROR          14  /* Failed to open or write log file      */
#define EXIT_THREAD_ERROR       15  /* Failed to create or join thread       */
#define EXIT_SIGNAL_ERROR       16  /* Failed to install signal handler      */


/* ─────────────────────────────────────────────────────────
 * PROCESS ROLE IDENTIFIERS
 * Used in log prefixes to identify which process logged a line
 * ───────────────────────────────────────────────────────── */
#define ROLE_CONTROLLER     "CONTROLLER"
#define ROLE_BOARD_GEN      "BOARD-GEN"
#define ROLE_STATION_1      "STATION-1 "
#define ROLE_STATION_2      "STATION-2 "
#define ROLE_STATION_3      "STATION-3 "
#define ROLE_DASHBOARD      "DASHBOARD "


/* ─────────────────────────────────────────────────────────
 * UTILITY MACROS
 * ───────────────────────────────────────────────────────── */

/* Print an error with file/line context and errno description */
#define SYS_ERR(msg)    do {                                        \
    fprintf(stderr, "[ERR] %s:%d — %s: %s\n",                      \
            __FILE__, __LINE__, (msg), strerror(errno));            \
} while(0)

/* Check syscall return, print error and jump to label on failure */
#define CHECK(ret, msg, label)  do {                                \
    if ((ret) < 0) {                                                \
        SYS_ERR(msg);                                               \
        goto label;                                                 \
    }                                                               \
} while(0)

/* Check pointer return (e.g. shmat, mmap) */
#define CHECK_PTR(ptr, msg, label)  do {                            \
    if ((ptr) == (void*)-1 || (ptr) == NULL) {                      \
        SYS_ERR(msg);                                               \
        goto label;                                                 \
    }                                                               \
} while(0)

#endif /* IPC_CONFIG_H */
