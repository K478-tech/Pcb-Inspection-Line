/* =============================================================================
 * board.h — Core Data Structures for PCB Quality Inspection Line
 * =============================================================================
 * Defines all shared data types used across processes and threads:
 *   - board_t        : represents one PCB board on the line
 *   - solder_result_t: result from Station 1 (solder inspection)
 *   - placement_result_t: result from Station 2 (placement check)
 *   - continuity_result_t: result from Station 3 (continuity test)
 *   - board_record_t : full per-board log record (all 3 results combined)
 *   - stats_t        : live shared memory statistics block
 *   - alert_msg_t    : message queue alert packet
 * =============================================================================
 */

#ifndef BOARD_H
#define BOARD_H

#include <time.h>
#include <stdint.h>

/* ─────────────────────────────────────────────────────────
 * CONSTANTS
 * ───────────────────────────────────────────────────────── */

#define MAX_BOARDS          500     /* Max boards in one simulation run      */
#define MAX_SOLDER_JOINTS   32      /* Joints per board checked by Station 1 */
#define MAX_COMPONENTS      16      /* Components checked by Station 2       */
#define MAX_NETS            24      /* Electrical nets checked by Station 3  */
#define BOARD_ID_LEN        12      /* e.g. "PCB-00042"                      */

/* Defect thresholds */
#define SOLDER_MIN_QUALITY  70      /* Below this = cold solder joint        */
#define PLACEMENT_MAX_OFFSET 2.5f  /* mm — max allowed placement error      */
#define CONTINUITY_MIN_RES  0.1f   /* ohms — below = short circuit          */
#define CONTINUITY_MAX_RES  9999.0f/* ohms — above = open circuit           */

/* Verdict codes */
#define VERDICT_PASS        0
#define VERDICT_FAIL_SOLDER 1
#define VERDICT_FAIL_PLACE  2
#define VERDICT_FAIL_CONT   3
#define VERDICT_FAIL_MULTI  4       /* Failed at more than one station       */

/* ─────────────────────────────────────────────────────────
 * BOARD STRUCT
 * Represents a single PCB board entering the inspection line
 * ───────────────────────────────────────────────────────── */
typedef struct {
    int         board_id;                   /* Unique sequential ID          */
    char        board_name[BOARD_ID_LEN];   /* Human-readable e.g. PCB-00042 */
    int         num_solder_joints;          /* How many joints to inspect    */
    int         num_components;             /* How many components to check  */
    int         num_nets;                   /* How many nets to test         */
    time_t      entry_time;                 /* When board entered the line   */
    uint8_t     simulated_defect_flag;      /* 1 = board has planted defect  */
} board_t;


/* ─────────────────────────────────────────────────────────
 * STATION 1 RESULT — Solder Inspection
 * Each joint gets a quality score 0-100
 * ───────────────────────────────────────────────────────── */
typedef struct {
    int     joint_scores[MAX_SOLDER_JOINTS]; /* Quality score per joint      */
    int     num_joints_checked;              /* Actual joints checked        */
    int     num_defective_joints;            /* Joints below threshold       */
    int     worst_joint_index;              /* Index of worst joint          */
    int     worst_joint_score;             /* Score of worst joint           */
    uint8_t station_verdict;               /* VERDICT_PASS or VERDICT_FAIL_SOLDER */
} solder_result_t;


/* ─────────────────────────────────────────────────────────
 * STATION 2 RESULT — Component Placement Check
 * Each component has an X/Y offset from its target position
 * ───────────────────────────────────────────────────────── */
typedef struct {
    float   x_offset[MAX_COMPONENTS];   /* Horizontal deviation in mm       */
    float   y_offset[MAX_COMPONENTS];   /* Vertical deviation in mm         */
    int     num_components_checked;     /* Actual components checked        */
    int     num_misplaced;              /* Components exceeding max offset   */
    int     worst_component_index;      /* Index of most misplaced part     */
    float   worst_offset_mm;           /* Largest offset found (mm)         */
    uint8_t station_verdict;           /* VERDICT_PASS or VERDICT_FAIL_PLACE*/
} placement_result_t;


/* ─────────────────────────────────────────────────────────
 * STATION 3 RESULT — Electrical Continuity Test
 * Each net is checked for open circuits or short circuits
 * ───────────────────────────────────────────────────────── */
typedef struct {
    float   resistance[MAX_NETS];       /* Measured resistance per net (ohm) */
    int     num_nets_checked;           /* Actual nets tested               */
    int     num_opens;                  /* Nets with resistance > MAX_RES   */
    int     num_shorts;                 /* Nets with resistance < MIN_RES   */
    int     worst_net_index;            /* Index of most faulty net         */
    float   worst_resistance;          /* Resistance of worst net           */
    uint8_t station_verdict;           /* VERDICT_PASS or VERDICT_FAIL_CONT */
} continuity_result_t;


/* ─────────────────────────────────────────────────────────
 * BOARD RECORD — Written to inspection_log.bin
 * One record per board, appended after Station 3 completes
 * ───────────────────────────────────────────────────────── */
typedef struct {
    board_t             board;          /* Original board info              */
    solder_result_t     solder;         /* Station 1 result                 */
    placement_result_t  placement;      /* Station 2 result                 */
    continuity_result_t continuity;     /* Station 3 result                 */
    uint8_t             final_verdict;  /* VERDICT_PASS or VERDICT_FAIL_*   */
    time_t              exit_time;      /* When board completed all stations*/
    double              total_time_sec; /* Total inspection time in seconds */
} board_record_t;


/* ─────────────────────────────────────────────────────────
 * LOG FILE HEADER — First 64 bytes of inspection_log.bin
 * Updated via lseek() after every board completes
 * ───────────────────────────────────────────────────────── */
typedef struct {
    uint32_t    magic;                  /* 0xPCB12345 — file integrity check */
    uint32_t    version;                /* Log format version = 1           */
    int32_t     total_boards;           /* Total boards logged so far       */
    int32_t     total_pass;             /* Total boards that passed         */
    int32_t     total_fail;             /* Total boards that failed         */
    int32_t     fail_solder;            /* Failures at Station 1            */
    int32_t     fail_placement;         /* Failures at Station 2            */
    int32_t     fail_continuity;        /* Failures at Station 3            */
    time_t      run_start_time;         /* Simulation start timestamp       */
    time_t      last_update_time;       /* When header was last rewritten   */
    uint8_t     reserved[8];            /* Padding to reach 64 bytes        */
} log_header_t;

/* Compile-time check: header must be exactly 64 bytes */
_Static_assert(sizeof(log_header_t) == 64, "log_header_t must be 64 bytes");


/* ─────────────────────────────────────────────────────────
 * SHARED MEMORY STATS BLOCK — stats_t
 * Written by all station processes, read by Dashboard
 * Protected by a POSIX named semaphore
 * ───────────────────────────────────────────────────────── */
typedef struct {
    /* Throughput counters */
    int     boards_at_station[3];       /* How many boards each stn has seen */
    int     boards_completed;           /* Boards that finished all 3 stns  */

    /* Defect counters */
    int     defects_station[3];         /* Defect count per station         */
    int     total_defects;              /* Overall defect count             */

    /* Rate metrics (updated every SIGALRM cycle) */
    float   defect_rate_percent;        /* total_defects / boards_completed */
    float   throughput_per_min;         /* boards completed per minute      */

    /* Per-station health */
    float   avg_solder_score;           /* Rolling average solder quality   */
    float   avg_placement_offset_mm;    /* Rolling average placement error  */
    float   avg_continuity_resistance;  /* Rolling average resistance       */

    /* Timestamps */
    time_t  last_board_time;            /* Time last board was completed    */
    time_t  stats_updated_at;           /* When this block was last written */
} stats_t;


/* ─────────────────────────────────────────────────────────
 * MESSAGE QUEUE ALERT PACKET — alert_msg_t
 * Sent by Station 3 → Line Controller via msgsnd/msgrcv
 * ───────────────────────────────────────────────────────── */
typedef struct {
    long    msg_type;                   /* POSIX mtype: use 1 for all alerts*/
    int     board_id;                   /* Which board triggered the alert  */
    char    board_name[BOARD_ID_LEN];   /* Human-readable board name        */
    uint8_t alert_station;              /* Station number: 1, 2, or 3       */
    uint8_t defect_type;                /* VERDICT_FAIL_* code              */
    char    description[128];           /* Human-readable defect description*/
    time_t  alert_time;                 /* When alert was generated         */
} alert_msg_t;


/* ─────────────────────────────────────────────────────────
 * UTILITY MACROS
 * ───────────────────────────────────────────────────────── */

/* Convert verdict code to a readable string */
#define VERDICT_STR(v) (                        \
    (v) == VERDICT_PASS         ? "PASS"      : \
    (v) == VERDICT_FAIL_SOLDER  ? "FAIL-SLD"  : \
    (v) == VERDICT_FAIL_PLACE   ? "FAIL-PLC"  : \
    (v) == VERDICT_FAIL_CONT    ? "FAIL-CNT"  : \
    (v) == VERDICT_FAIL_MULTI   ? "FAIL-MLT"  : "UNKNOWN")

/* Station index to name */
#define STATION_NAME(n) (                           \
    (n) == 0 ? "Solder Inspector"     :             \
    (n) == 1 ? "Placement Checker"    :             \
    (n) == 2 ? "Continuity Tester"    : "Unknown")

#endif /* BOARD_H */
