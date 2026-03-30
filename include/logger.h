/* =============================================================================
 * logger.c — File I/O Logging Implementation
 * =============================================================================
 * Implements all logging functions declared in logger.h
 * Demonstrates: open, read, write, lseek, close
 * =============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../include/board.h"
#include "../include/ipc_config.h"
#include "../include/logger.h"

/* ─────────────────────────────────────────────────────────
 * UTILITY
 * ───────────────────────────────────────────────────────── */

void logger_format_timestamp(char *buf, size_t buf_len)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", t);
}

int logger_make_logdir(void)
{
    if (mkdir(LOG_DIR, 0755) < 0 && errno != EEXIST) {
        SYS_ERR("mkdir logs/");
        return -1;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────
 * INITIALIZATION
 * ───────────────────────────────────────────────────────── */

int logger_init(logger_ctx_t *ctx, const char *role)
{
    memset(ctx, 0, sizeof(logger_ctx_t));
    strncpy(ctx->role, role, sizeof(ctx->role) - 1);

    if (logger_make_logdir() < 0)
        return -1;

    /* Binary log: create fresh, read/write */
    ctx->bin_fd = open(LOG_BINARY_PATH,
                       O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (ctx->bin_fd < 0) {
        SYS_ERR("open inspection_log.bin");
        return -1;
    }

    /* Alerts log: append-only text */
    ctx->alert_fd = open(LOG_ALERTS_PATH,
                         O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (ctx->alert_fd < 0) {
        SYS_ERR("open alerts.log");
        close(ctx->bin_fd);
        return -1;
    }

    /* Write initial header to binary log */
    log_header_t header;
    memset(&header, 0, sizeof(log_header_t));
    header.magic           = 0xCB123456U;
    header.version         = 1;
    header.total_boards    = 0;
    header.total_pass      = 0;
    header.total_fail      = 0;
    header.run_start_time  = time(NULL);
    header.last_update_time = time(NULL);

    if (logger_write_header(ctx, &header) < 0)
        return -1;

    ctx->is_open = 1;

    char msg[128];
    snprintf(msg, sizeof(msg), "Logger initialized — binary log + alerts log opened");
    logger_write_event(ctx, role, msg);

    return 0;
}

int logger_open_append(logger_ctx_t *ctx, const char *role)
{
    memset(ctx, 0, sizeof(logger_ctx_t));
    strncpy(ctx->role, role, sizeof(ctx->role) - 1);

    /* Open binary log for reading and writing (no truncate) */
    ctx->bin_fd = open(LOG_BINARY_PATH, O_RDWR, 0644);
    if (ctx->bin_fd < 0) {
        SYS_ERR("open inspection_log.bin (append mode)");
        return -1;
    }

    /* Open alerts log for append */
    ctx->alert_fd = open(LOG_ALERTS_PATH,
                         O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (ctx->alert_fd < 0) {
        SYS_ERR("open alerts.log (append mode)");
        close(ctx->bin_fd);
        return -1;
    }

    ctx->is_open = 1;
    return 0;
}

void logger_close(logger_ctx_t *ctx)
{
    if (!ctx || !ctx->is_open)
        return;

    if (ctx->bin_fd > 0)   close(ctx->bin_fd);
    if (ctx->alert_fd > 0) close(ctx->alert_fd);

    ctx->is_open  = 0;
    ctx->bin_fd   = -1;
    ctx->alert_fd = -1;
}

/* ─────────────────────────────────────────────────────────
 * BINARY LOG — HEADER OPERATIONS
 * ───────────────────────────────────────────────────────── */

int logger_write_header(logger_ctx_t *ctx, const log_header_t *header)
{
    /* Seek to beginning of file */
    if (lseek(ctx->bin_fd, 0, SEEK_SET) < 0) {
        SYS_ERR("lseek to header (write)");
        return -1;
    }

    ssize_t written = write(ctx->bin_fd, header, sizeof(log_header_t));
    if (written != (ssize_t)sizeof(log_header_t)) {
        SYS_ERR("write log_header_t");
        return -1;
    }

    return 0;
}

int logger_update_header(logger_ctx_t *ctx, const log_header_t *header)
{
    /* KEY lseek USAGE: rewind to offset 0, overwrite header in-place */
    if (lseek(ctx->bin_fd, 0, SEEK_SET) < 0) {
        SYS_ERR("lseek to offset 0 for header update");
        return -1;
    }

    ssize_t written = write(ctx->bin_fd, header, sizeof(log_header_t));
    if (written != (ssize_t)sizeof(log_header_t)) {
        SYS_ERR("write updated header");
        return -1;
    }

    /* Seek back to end so next append goes to correct position */
    if (lseek(ctx->bin_fd, 0, SEEK_END) < 0) {
        SYS_ERR("lseek back to end after header update");
        return -1;
    }

    return 0;
}

int logger_read_header(logger_ctx_t *ctx, log_header_t *header)
{
    /* Save current position */
    off_t cur = lseek(ctx->bin_fd, 0, SEEK_CUR);
    if (cur < 0) {
        SYS_ERR("lseek save position");
        return -1;
    }

    /* Seek to start and read header */
    if (lseek(ctx->bin_fd, 0, SEEK_SET) < 0) {
        SYS_ERR("lseek to header (read)");
        return -1;
    }

    ssize_t rd = read(ctx->bin_fd, header, sizeof(log_header_t));
    if (rd != (ssize_t)sizeof(log_header_t)) {
        SYS_ERR("read log_header_t");
        return -1;
    }

    /* Restore position */
    lseek(ctx->bin_fd, cur, SEEK_SET);
    return 0;
}

/* ─────────────────────────────────────────────────────────
 * BINARY LOG — RECORD OPERATIONS
 * ───────────────────────────────────────────────────────── */

int logger_append_record(logger_ctx_t *ctx, const board_record_t *record)
{
    /* Seek to end of file to append */
    if (lseek(ctx->bin_fd, 0, SEEK_END) < 0) {
        SYS_ERR("lseek SEEK_END for record append");
        return -1;
    }

    ssize_t written = write(ctx->bin_fd, record, sizeof(board_record_t));
    if (written != (ssize_t)sizeof(board_record_t)) {
        SYS_ERR("write board_record_t");
        return -1;
    }

    /* Now update the header counters */
    log_header_t header;
    if (logger_read_header(ctx, &header) < 0)
        return -1;

    header.total_boards++;
    header.last_update_time = time(NULL);

    if (record->final_verdict == VERDICT_PASS) {
        header.total_pass++;
    } else {
        header.total_fail++;
        if (record->final_verdict == VERDICT_FAIL_SOLDER)    header.fail_solder++;
        else if (record->final_verdict == VERDICT_FAIL_PLACE) header.fail_placement++;
        else if (record->final_verdict == VERDICT_FAIL_CONT)  header.fail_continuity++;
        else if (record->final_verdict == VERDICT_FAIL_MULTI) {
            header.fail_solder++;
            header.fail_continuity++;
        }
    }

    /* lseek back to offset 0 and rewrite header */
    return logger_update_header(ctx, &header);
}

int logger_read_record(logger_ctx_t *ctx, int index, board_record_t *record)
{
    /* Calculate exact byte offset for this record */
    off_t offset = (off_t)sizeof(log_header_t)
                 + (off_t)(index * sizeof(board_record_t));

    if (lseek(ctx->bin_fd, offset, SEEK_SET) < 0) {
        SYS_ERR("lseek to record offset");
        return -1;
    }

    ssize_t rd = read(ctx->bin_fd, record, sizeof(board_record_t));
    if (rd != (ssize_t)sizeof(board_record_t)) {
        SYS_ERR("read board_record_t");
        return -1;
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────
 * TEXT ALERT LOG
 * ───────────────────────────────────────────────────────── */

int logger_write_alert(logger_ctx_t *ctx, const alert_msg_t *alert)
{
    char ts[32];
    logger_format_timestamp(ts, sizeof(ts));

    char line[512];
    int  len = snprintf(line, sizeof(line),
        "[%s] ALERT | Board %-10s | Stn%d | %s\n",
        ts,
        alert->board_name,
        alert->alert_station,
        alert->description);

    if (len < 0) return -1;

    ssize_t written = write(ctx->alert_fd, line, (size_t)len);
    if (written < 0) {
        SYS_ERR("write alert line");
        return -1;
    }

    return 0;
}

int logger_write_event(logger_ctx_t *ctx, const char *role, const char *message)
{
    char ts[32];
    logger_format_timestamp(ts, sizeof(ts));

    char line[512];
    int  len = snprintf(line, sizeof(line),
        "[%s] EVENT | %-12s | %s\n", ts, role, message);

    if (len < 0) return -1;

    ssize_t written = write(ctx->alert_fd, line, (size_t)len);
    if (written < 0) {
        SYS_ERR("write event line");
        return -1;
    }

    return 0;
}

/* ─────────────────────────────────────────────────────────
 * SNAPSHOT REPORT
 * ───────────────────────────────────────────────────────── */

int logger_write_snapshot(const stats_t *stats, const log_header_t *header)
{
    int fd = open(LOG_SNAPSHOT_PATH,
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        SYS_ERR("open snapshot_report.txt");
        return -1;
    }

    char ts[32];
    logger_format_timestamp(ts, sizeof(ts));

    char buf[2048];
    int  len = snprintf(buf, sizeof(buf),
        "========================================\n"
        "  PCB INSPECTION LINE — SNAPSHOT REPORT \n"
        "========================================\n"
        "Generated  : %s\n"
        "Run Start  : %s\n"
        "\n--- THROUGHPUT ---\n"
        "Boards In  : %d / %d / %d  (Stn1/Stn2/Stn3)\n"
        "Completed  : %d\n"
        "Rate       : %.2f boards/min\n"
        "\n--- QUALITY ---\n"
        "Total Defects     : %d\n"
        "Defect Rate       : %.2f%%\n"
        "Defects Stn1      : %d  (Solder)\n"
        "Defects Stn2      : %d  (Placement)\n"
        "Defects Stn3      : %d  (Continuity)\n"
        "\n--- AVERAGES ---\n"
        "Avg Solder Score  : %.1f / 100\n"
        "Avg Place Offset  : %.2f mm\n"
        "Avg Continuity    : %.2f ohm\n"
        "\n--- FILE LOG TOTALS ---\n"
        "Total Logged      : %d\n"
        "Pass              : %d\n"
        "Fail              : %d\n"
        "  Fail-Solder     : %d\n"
        "  Fail-Placement  : %d\n"
        "  Fail-Continuity : %d\n"
        "========================================\n",
        ts,
        ctime(&header->run_start_time),
        stats->boards_at_station[0],
        stats->boards_at_station[1],
        stats->boards_at_station[2],
        stats->boards_completed,
        stats->throughput_per_min,
        stats->total_defects,
        stats->defect_rate_percent,
        stats->defects_station[0],
        stats->defects_station[1],
        stats->defects_station[2],
        stats->avg_solder_score,
        stats->avg_placement_offset_mm,
        stats->avg_continuity_resistance,
        header->total_boards,
        header->total_pass,
        header->total_fail,
        header->fail_solder,
        header->fail_placement,
        header->fail_continuity);

    ssize_t w = write(fd, buf, (size_t)len);
    if (w < 0) SYS_ERR("write snapshot");
    close(fd);

    printf("[LOGGER] Snapshot written to %s\n", LOG_SNAPSHOT_PATH);
    return (w < 0) ? -1 : 0;
}
