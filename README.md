# PCB Quality Inspection Line

---

## Scenario Summary

A simulated PCB (Printed Circuit Board) manufacturing quality inspection line.
Boards flow through three sequential inspection stations — each running as a
separate process — before a final verdict (PASS/FAIL) is issued and logged.

A central **Line Controller** (parent process) supervises everything:
spawns children, handles signals, reads defect alerts, and cleans up all IPC
resources on shutdown.

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                    LINE CONTROLLER (main)                           │
│   SIGINT→shutdown  SIGALRM→log  SIGUSR1→snapshot  SIGCHLD→reap     │
│   msgrcv() ← Message Queue ← Station3 alerts                       │
│   reads Shared Memory for final summary                             │
└──────────────┬──────────────────────────────────────────────────────┘
               │ fork() + exec()
   ┌───────────┬───────────┬───────────┬──────────────┐
   │           │           │           │              │
[board_gen] [station1] [station2] [station3]    [dashboard]
   │           │           │           │              │
   │  FIFO_0   │  FIFO_1   │  FIFO_2   │  MsgQueue    │
   └──────────►└──────────►└──────────►└─────────►[CTRL]
                                        │
                              SharedMem + Semaphore
                                        ▼
                                  [dashboard]
                                  (live stats)
```

Each station has **3 internal threads**:
- **T1 Reader** — reads board data from upstream FIFO
- **T2 Inspector** — runs defect detection (protected by mutex + cond var)
- **T3 Writer** — forwards result to downstream FIFO / writes log

---

## File Structure

```
pcb_inspection/
├── Makefile
├── README.md
├── include/
│   ├── board.h          # All data structs (board_t, stats_t, alert_msg_t …)
│   ├── ipc_config.h     # FIFO paths, keys, simulation parameters, macros
│   └── logger.h         # File I/O API (open/read/write/lseek/close)
├── src/
│   ├── main.c           # Line Controller — parent process
│   ├── board_gen.c      # Board data generator
│   ├── station1.c       # Solder inspection + 3 threads
│   ├── station2.c       # Placement check + 3 threads
│   ├── station3.c       # Continuity test + msg queue + log writer
│   ├── dashboard.c      # Live shared memory stats viewer
│   └── logger.c         # File I/O implementation
├── bin/                 # Built binaries (created by make)
└── logs/                # Log output (created at runtime)
    ├── inspection_log.bin
    ├── alerts.log
    └── snapshot_report.txt
```

---

## Build Steps

```bash
# Clone / extract the project
cd pcb_inspection

# Build all binaries
make all

# Run the simulation
make run

# Quick test (10 boards)
make test

# Clean everything
make clean

# Force kill lingering processes
make kill
```

**Requirements:** GCC, POSIX-compliant Linux (Ubuntu 20.04+), `libpthread`, `librt`, `libm`

---

## Run Instructions

```bash
make run
```

The simulation will:
1. Create FIFOs, shared memory, semaphore, and message queue
2. Spawn 5 child processes
3. Generate and inspect 50 PCB boards through the pipeline
4. Display a live dashboard in the terminal
5. Print a final summary and clean up all IPC resources

**To trigger a snapshot report mid-run:**
```bash
kill -USR1 <PID_of_main>
# Check logs/snapshot_report.txt
```

**To gracefully shut down early:**
```bash
Ctrl+C   # or: kill -INT <PID_of_main>
```

---

## Where Each System Call Is Used

| System Call | File | Purpose |
|---|---|---|
| `fork` | `main.c` | Spawn each child process |
| `exec` (`execl`) | `main.c` | Replace child image with station binary |
| `waitpid` | `main.c` | Reap exited children, prevent zombies |
| `sigaction` | `main.c`, all stations | Install custom signal handlers |
| `alarm` | `main.c` | Arm periodic SIGALRM for throughput logging |
| `mkfifo` | `main.c` | Create named pipes for board pipeline |
| `open` | All station files, `logger.c` | Open FIFOs and log files |
| `read` | All station files | Read board structs from FIFOs |
| `write` | All station files, `logger.c` | Write structs to FIFOs and log files |
| `lseek` | `logger.c` | Rewind to update log header in-place |
| `close` | All files | Close FDs on exit |
| `shmget` | `main.c`, all stations | Create/attach shared memory segment |
| `shmat` / `shmdt` | All files | Map/unmap shared memory |
| `shmctl(IPC_RMID)` | `main.c` | Remove shared memory on shutdown |
| `sem_open` | `main.c`, all stations | Open POSIX named semaphore |
| `sem_wait` / `sem_post` | All stations, `dashboard.c` | Protect shared memory writes |
| `sem_unlink` | `main.c` | Remove semaphore on shutdown |
| `msgget` | `main.c`, `station3.c` | Create/open System V message queue |
| `msgsnd` | `station3.c` | Send defect alert to controller |
| `msgrcv` | `main.c` | Receive defect alerts from queue |
| `msgctl(IPC_RMID)` | `main.c` | Remove message queue on shutdown |
| `pthread_create` | All station files | Launch 3 threads per station |
| `pthread_join` | All station files | Wait for threads to finish |
| `pthread_mutex_lock/unlock` | All station files | Protect internal board queues |
| `pthread_cond_wait/signal` | All station files | Signal between Reader→Inspector |

---

## Sample Run Output

```
╔══════════════════════════════════════════╗
║  PCB Quality Inspection Line Controller  ║
║  PID: 12345                              ║
╚══════════════════════════════════════════╝

[CONTROLLER] Signal handlers installed
[CONTROLLER] Logger initialized
[CONTROLLER] FIFO created: /tmp/pcb_fifo_0
[CONTROLLER] FIFO created: /tmp/pcb_fifo_1
[CONTROLLER] FIFO created: /tmp/pcb_fifo_2
[CONTROLLER] Shared memory created (id=5, size=128 bytes)
[CONTROLLER] Semaphore created: /pcb_shm_sem
[CONTROLLER] Message queue created (id=3)
[CONTROLLER] Spawned station1    (PID=12346)
[CONTROLLER] Spawned station2    (PID=12347)
[CONTROLLER] Spawned station3    (PID=12348)
[CONTROLLER] Spawned dashboard   (PID=12349)
[CONTROLLER] Spawned board_gen   (PID=12350)
[BOARD-GEN ] Board PCB-00001  | joints=24 comps=12 nets=18
[STATION-1 ] T2 done board PCB-00001 | verdict=PASS defects=0
[STATION-2 ] T2 done board PCB-00001 | verdict=PASS misplaced=0
[STATION-3 ] T2 done board PCB-00001 | FINAL=PASS opens=0 shorts=0
...
[CONTROLLER] ALERT received | Board PCB-00007  | Verdict=FAIL-MLT | ...

╔══════════════════════════════════════════╗
║       FINAL INSPECTION SUMMARY           ║
╠══════════════════════════════════════════╣
║  Total Boards Logged : 50                ║
║  Passed              : 43                ║
║  Failed              : 7                 ║
║    Fail-Solder       : 3                 ║
║    Fail-Placement    : 2                 ║
║    Fail-Continuity   : 2                 ║
╚══════════════════════════════════════════╝

[CONTROLLER] All resources cleaned up. Goodbye.
```

---

## Design Highlights

- **No hardware** — all board data is randomly generated with a fixed seed (`SIM_RANDOM_SEED=42`) for reproducible runs
- **Clean shutdown** — SIGINT triggers graceful stop: SIGUSR2 to children → SIGTERM → waitpid → IPC cleanup → no zombies
- **lseek showcase** — `logger_update_header()` in `logger.c` rewinds to byte 0 to update counters in-place without rewriting records
- **Sentinel pattern** — a zeroed `board_t` (board_id=0) signals end-of-stream through all FIFOs without requiring extra synchronization
- **Thread safety** — every station's internal queue is protected by a dedicated `pthread_mutex_t` and `pthread_cond_t` pair

---
