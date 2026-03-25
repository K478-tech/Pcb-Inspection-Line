# =============================================================================
# Makefile — PCB Quality Inspection Line
# =============================================================================
# Targets:
#   make all     — build all binaries
#   make clean   — remove all build artifacts
#   make run     — run the full simulation
#   make test    — run with a short board count for quick testing
#   make kill    — kill any lingering simulation processes
# =============================================================================

CC      := gcc
CFLAGS  := -Wall -Wextra -Wpedantic -g -O2 \
           -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L
LDFLAGS := -lpthread -lrt -lm

# Directories
SRC_DIR := src
INC_DIR := include
BIN_DIR := bin
LOG_DIR := logs

# Source files → binary mapping
SOURCES := main board_gen station1 station2 station3 dashboard logger

# All binaries (except logger — it's a library compiled into each binary)
BINARIES := $(BIN_DIR)/main    \
            $(BIN_DIR)/board_gen \
            $(BIN_DIR)/station1  \
            $(BIN_DIR)/station2  \
            $(BIN_DIR)/station3  \
            $(BIN_DIR)/dashboard

# Header dependencies
HEADERS := $(INC_DIR)/board.h \
           $(INC_DIR)/ipc_config.h \
           $(INC_DIR)/logger.h

# =============================================================================
# Default target
# =============================================================================

.PHONY: all
all: $(BIN_DIR) $(LOG_DIR) $(BINARIES)
	@echo ""
	@echo "  ✓ Build complete — all binaries in ./bin/"
	@echo "  Run with: make run"
	@echo ""

# =============================================================================
# Create output directories
# =============================================================================

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(LOG_DIR):
	@mkdir -p $(LOG_DIR)

# =============================================================================
# Binary build rules
# Each binary links with logger.c for file I/O support
# =============================================================================

$(BIN_DIR)/main: $(SRC_DIR)/main.c $(SRC_DIR)/logger.c $(HEADERS)
	$(CC) $(CFLAGS) -I$(INC_DIR) \
	      $(SRC_DIR)/main.c $(SRC_DIR)/logger.c \
	      -o $@ $(LDFLAGS)
	@echo "  Built: $@"

$(BIN_DIR)/board_gen: $(SRC_DIR)/board_gen.c $(SRC_DIR)/logger.c $(HEADERS)
	$(CC) $(CFLAGS) -I$(INC_DIR) \
	      $(SRC_DIR)/board_gen.c $(SRC_DIR)/logger.c \
	      -o $@ $(LDFLAGS)
	@echo "  Built: $@"

$(BIN_DIR)/station1: $(SRC_DIR)/station1.c $(SRC_DIR)/logger.c $(HEADERS)
	$(CC) $(CFLAGS) -I$(INC_DIR) \
	      $(SRC_DIR)/station1.c $(SRC_DIR)/logger.c \
	      -o $@ $(LDFLAGS)
	@echo "  Built: $@"

$(BIN_DIR)/station2: $(SRC_DIR)/station2.c $(SRC_DIR)/logger.c $(HEADERS)
	$(CC) $(CFLAGS) -I$(INC_DIR) \
	      $(SRC_DIR)/station2.c $(SRC_DIR)/logger.c \
	      -o $@ $(LDFLAGS)
	@echo "  Built: $@"

$(BIN_DIR)/station3: $(SRC_DIR)/station3.c $(SRC_DIR)/logger.c $(HEADERS)
	$(CC) $(CFLAGS) -I$(INC_DIR) \
	      $(SRC_DIR)/station3.c $(SRC_DIR)/logger.c \
	      -o $@ $(LDFLAGS)
	@echo "  Built: $@"

$(BIN_DIR)/dashboard: $(SRC_DIR)/dashboard.c $(SRC_DIR)/logger.c $(HEADERS)
	$(CC) $(CFLAGS) -I$(INC_DIR) \
	      $(SRC_DIR)/dashboard.c $(SRC_DIR)/logger.c \
	      -o $@ $(LDFLAGS)
	@echo "  Built: $@"

# =============================================================================
# Run targets
# =============================================================================

.PHONY: run
run: all
	@echo ""
	@echo "  ► Starting PCB Inspection Line simulation..."
	@echo "  ► Press Ctrl+C for graceful shutdown"
	@echo "  ► Send SIGUSR1 to PID for snapshot: kill -USR1 <PID>"
	@echo ""
	./$(BIN_DIR)/main

.PHONY: test
test: all
	@echo ""
	@echo "  ► Test run (10 boards, fast timing)..."
	@echo ""
	@# Override board count and timing via env for test mode
	SIM_BOARD_COUNT=10 ./$(BIN_DIR)/main

# =============================================================================
# Clean
# =============================================================================

.PHONY: clean
clean:
	@echo "  Removing binaries..."
	@rm -rf $(BIN_DIR)
	@echo "  Removing log files..."
	@rm -rf $(LOG_DIR)
	@echo "  Removing stale FIFOs..."
	@rm -f /tmp/pcb_fifo_0 /tmp/pcb_fifo_1 /tmp/pcb_fifo_2
	@echo "  ✓ Clean complete"

# =============================================================================
# Kill lingering processes
# =============================================================================

.PHONY: kill
kill:
	@echo "  Killing any running simulation processes..."
	@-pkill -f bin/main      2>/dev/null || true
	@-pkill -f bin/board_gen 2>/dev/null || true
	@-pkill -f bin/station1  2>/dev/null || true
	@-pkill -f bin/station2  2>/dev/null || true
	@-pkill -f bin/station3  2>/dev/null || true
	@-pkill -f bin/dashboard 2>/dev/null || true
	@-rm -f /tmp/pcb_fifo_0 /tmp/pcb_fifo_1 /tmp/pcb_fifo_2
	@echo "  ✓ Done"

# =============================================================================
# Help
# =============================================================================

.PHONY: help
help:
	@echo ""
	@echo "  PCB Quality Inspection Line — Makefile Help"
	@echo "  ─────────────────────────────────────────────"
	@echo "  make all     Build all binaries"
	@echo "  make run     Build and run simulation"
	@echo "  make test    Quick run with 10 boards"
	@echo "  make clean   Remove all build + log artifacts"
	@echo "  make kill    Force-kill any running processes"
	@echo "  make help    Show this message"
	@echo ""
