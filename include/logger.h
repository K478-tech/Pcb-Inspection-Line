/* =============================================================================
 * logger.h — File I/O Logging API for PCB Quality Inspection Line
 * =============================================================================
 * Declares all logging functions used across the system.
 * Implementation is in src/logger.c
 *
 * Two log files are maintained:
 *
 *   1. inspection_log.bin  — Binary structured log
 *      Layout:
 *        [log_header_t : 64 bytes]        ← seeked back & updated each board
 *        [board_record_t : record 0]
 *        [board_record_t : record 1]
 *        ...
 *      Uses: open, write, lseek, read, close
 *
 *   2. alerts.log  — Human-readable text alert log
 *      Each line: [TIMESTAMP] ALERT — board info — defect description
 *      Uses: open (O_APPEND), write, close
 *
 *   3. snapshot_report.txt — Generated on SIGUSR1
 *      Dumps current stats_t shared memory block to a readable report.
 *      Uses: open, write, close
 * =============================================================================
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include "board.h"
#include "ipc_config.h"

/* ─────────────────────────────────────────────────────────
 * LOGGER CONTEXT
 * Holds open file descriptors for the logging session.
 * One logger_ctx_t is created per process that needs logging.
 * ───────────────────────────────────────────────────────── */
typedef struct {
    int     bin_fd;         /* FD for inspection_log.bin (binary)           */
    int     alert_fd;       /* FD for alerts.log (text, O_APPEND)           */
    int     is_open;        /* 1 if logger was successfully initialized      */
    char    role[16];       /* Which process owns this logger e.g. "STN-1"  */
} logger_ctx_t;


/* ─────────────────────────────────────────────────────────
 * INITIALIZATION & TEARDOWN
 * ───────────────────────────────────────────────────────── */

/*
 * logger_init() — Open log files and write/verify the binary log header.
 *
 * Must be called ONCE by the Line Controller (parent) before any children
 * are forked. Creates the logs/ directory if it does not exist.
 *
 * Parameters:
 *   ctx   — pointer to logger_ctx_t to initialize
 *   role  — string identifier for this process (e.g. ROLE_CONTROLLER)
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_init(logger_ctx_t *ctx, const char *role);

/*
 * logger_open_append() — Open an existing log for appending records.
 *
 * Called by child station processes AFTER the parent has created the log.
 * These processes only append board_record_t entries — they never rewrite
 * the header (only the parent does that via logger_update_header).
 *
 * Parameters:
 *   ctx   — pointer to logger_ctx_t to initialize
 *   role  — string identifier for this process
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_open_append(logger_ctx_t *ctx, const char *role);

/*
 * logger_close() — Flush and close all open file descriptors.
 *
 * Must be called before process exit to avoid fd leaks.
 *
 * Parameters:
 *   ctx — pointer to logger_ctx_t to close
 */
void logger_close(logger_ctx_t *ctx);


/* ─────────────────────────────────────────────────────────
 * BINARY LOG OPERATIONS (inspection_log.bin)
 * ───────────────────────────────────────────────────────── */

/*
 * logger_write_header() — Write initial log_header_t to start of binary log.
 *
 * Uses lseek(fd, 0, SEEK_SET) to rewind to offset 0 before writing.
 * Called once during logger_init().
 *
 * Parameters:
 *   ctx    — initialized logger context
 *   header — pointer to the log_header_t to write
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_write_header(logger_ctx_t *ctx, const log_header_t *header);

/*
 * logger_update_header() — Rewrite the header in-place after each board.
 *
 * Uses lseek(fd, 0, SEEK_SET) to jump back to offset 0, overwrites header,
 * then seeks back to end of file for the next record append.
 *
 * This is the KEY lseek demonstration — updating metadata in-place without
 * rewriting the entire file.
 *
 * Parameters:
 *   ctx    — initialized logger context
 *   header — updated log_header_t to write back
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_update_header(logger_ctx_t *ctx, const log_header_t *header);

/*
 * logger_read_header() — Read and return the current log file header.
 *
 * Seeks to offset 0, reads log_header_t, seeks back to end.
 * Used during SIGUSR1 snapshot to get current totals.
 *
 * Parameters:
 *   ctx    — initialized logger context
 *   header — output: pointer to log_header_t to fill
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_read_header(logger_ctx_t *ctx, log_header_t *header);

/*
 * logger_append_record() — Append a completed board_record_t to the log.
 *
 * Seeks to end of file and writes the full board_record_t struct.
 * After writing, calls logger_update_header() to refresh totals.
 *
 * Parameters:
 *   ctx    — initialized logger context
 *   record — fully populated board_record_t to append
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_append_record(logger_ctx_t *ctx, const board_record_t *record);

/*
 * logger_read_record() — Read a board_record_t by board index (0-based).
 *
 * Calculates offset: sizeof(log_header_t) + (index * sizeof(board_record_t))
 * Uses lseek() to jump directly to that record.
 * Used during snapshots and post-run analysis.
 *
 * Parameters:
 *   ctx    — initialized logger context
 *   index  — 0-based record index
 *   record — output: pointer to board_record_t to fill
 *
 * Returns: 0 on success, -1 on failure (including index out of range)
 */
int logger_read_record(logger_ctx_t *ctx, int index, board_record_t *record);


/* ─────────────────────────────────────────────────────────
 * TEXT ALERT LOG OPERATIONS (alerts.log)
 * ───────────────────────────────────────────────────────── */

/*
 * logger_write_alert() — Append a formatted defect alert to alerts.log.
 *
 * Formats a human-readable line and writes it using write() with O_APPEND.
 * Example output:
 *   [2025-03-09 10:22:01] ALERT | Board PCB-00042 | Stn1-Solder |
 *   3 cold joints (worst: joint #7, score=45)
 *
 * Parameters:
 *   ctx   — initialized logger context
 *   alert — alert_msg_t to format and write
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_write_alert(logger_ctx_t *ctx, const alert_msg_t *alert);

/*
 * logger_write_event() — Write a plain text event line to alerts.log.
 *
 * Used for system events like startup, shutdown, signal received.
 * Example:
 *   [2025-03-09 10:30:00] EVENT | SIGINT received — initiating shutdown
 *
 * Parameters:
 *   ctx     — initialized logger context
 *   role    — process role string
 *   message — event description string (max 256 chars)
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_write_event(logger_ctx_t *ctx, const char *role, const char *message);


/* ─────────────────────────────────────────────────────────
 * SNAPSHOT REPORT (snapshot_report.txt)
 * ───────────────────────────────────────────────────────── */

/*
 * logger_write_snapshot() — Dump current stats to snapshot_report.txt
 *
 * Triggered by SIGUSR1 signal handler in Line Controller.
 * Opens snapshot file (O_WRONLY | O_CREAT | O_TRUNC), writes a
 * human-readable summary of the stats_t block, then closes.
 *
 * Parameters:
 *   stats  — pointer to current stats_t from shared memory
 *   header — pointer to current log_header_t from binary log
 *
 * Returns: 0 on success, -1 on failure
 */
int logger_write_snapshot(const stats_t *stats, const log_header_t *header);


/* ─────────────────────────────────────────────────────────
 * UTILITY
 * ───────────────────────────────────────────────────────── */

/*
 * logger_format_timestamp() — Fill buf with a formatted timestamp string.
 *
 * Output format: "2025-03-09 10:22:01"
 * Uses time() and localtime() internally.
 *
 * Parameters:
 *   buf     — output buffer (must be at least 20 bytes)
 *   buf_len — size of the buffer
 */
void logger_format_timestamp(char *buf, size_t buf_len);

/*
 * logger_make_logdir() — Create the logs/ directory if it doesn't exist.
 *
 * Uses mkdir() with mode 0755. Ignores EEXIST error.
 *
 * Returns: 0 on success, -1 on failure (other than EEXIST)
 */
int logger_make_logdir(void);


/* ─────────────────────────────────────────────────────────
 * FILE LAYOUT REFERENCE (for documentation)
 *
 *  inspection_log.bin layout:
 *  ┌─────────────────────────────────┐  Offset 0
 *  │  log_header_t  (64 bytes)       │  ← lseek here to update counters
 *  ├─────────────────────────────────┤  Offset 64
 *  │  board_record_t  record[0]      │
 *  ├─────────────────────────────────┤  Offset 64 + 1*sizeof(board_record_t)
 *  │  board_record_t  record[1]      │
 *  ├─────────────────────────────────┤
 *  │  ...                            │
 *  └─────────────────────────────────┘
 *
 *  To seek to record[n]:
 *    lseek(fd, sizeof(log_header_t) + n * sizeof(board_record_t), SEEK_SET)
 * ───────────────────────────────────────────────────────── */

#endif /* LOGGER_H */
