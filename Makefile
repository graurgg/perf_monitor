CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wshadow -std=gnu11 -D_GNU_SOURCE
LDFLAGS ?=

BIN      := tracer
BENCHES  := bench/linear bench/random bench/pointer_chase bench/multi_object

.PHONY: all bench clean run-bench snapshots

all: $(BIN) $(BENCHES)

$(BIN): src/tracer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench/%: bench/%.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench: $(BENCHES)

# Convenience target. Pipes the tracer through the plotter so a single
# `make run-bench` shows the live window. Override PERIOD or SIZE_MB on
# the command line, e.g. `make run-bench PERIOD=5000 SIZE_MB=256`.
PERIOD  ?= 10000
SIZE_MB ?= 64
run-bench: all
	./$(BIN) -p $(PERIOD) -- ./bench/linear --size-mb $(SIZE_MB) | ./plot.py

# Render one PNG per bench for the writeup. Each invocation runs the
# bench under the tracer and pipes the stream into plot.py --snapshot,
# which renders one frame after EOF and exits. Needs sudo because IBS
# perf_event_open requires CAP_PERFMON / paranoid<=2.
#
# Override SNAPSHOT_DIR or per-bench knobs on the command line, e.g.:
#   make snapshots SNAPSHOT_DIR=docs/figs LINEAR_MB=512
SNAPSHOT_DIR ?= screenshots
SNAP_PERIOD  ?= 5000
LINEAR_MB    ?= 256
RANDOM_MB    ?= 128
PCHASE_MB    ?= 64
MULTI_MB     ?= 32

snapshots: all
	@mkdir -p $(SNAPSHOT_DIR)
	@echo "[1/4] linear ($(LINEAR_MB) MiB sequential)"
	sudo ./$(BIN) -p $(SNAP_PERIOD) -- ./bench/linear \
	    --size-mb=$(LINEAR_MB) --iters=200 --mode=rw \
	    | ./plot.py --snapshot $(SNAPSHOT_DIR)/linear.png \
	                --window-s 120 \
	                --title "bench/linear  $(LINEAR_MB) MiB sequential rw"
	@echo "[2/4] random ($(RANDOM_MB) MiB shuffled)"
	sudo ./$(BIN) -p $(SNAP_PERIOD) -- ./bench/random \
	    --size-mb=$(RANDOM_MB) --iters=20 --mode=rw \
	    | ./plot.py --snapshot $(SNAPSHOT_DIR)/random.png \
	                --window-s 120 \
	                --title "bench/random  $(RANDOM_MB) MiB shuffled rw"
	@echo "[3/4] pointer_chase ($(PCHASE_MB) MiB cyclic)"
	sudo ./$(BIN) -p $(SNAP_PERIOD) -- ./bench/pointer_chase \
	    --size-mb=$(PCHASE_MB) --iters=40 \
	    | ./plot.py --snapshot $(SNAPSHOT_DIR)/pointer_chase.png \
	                --window-s 120 \
	                --title "bench/pointer_chase  $(PCHASE_MB) MiB cycle"
	@echo "[4/4] multi_object (heap + stack + tmpfile)"
	sudo ./$(BIN) -p $(SNAP_PERIOD) -- ./bench/multi_object \
	    --size-mb=$(MULTI_MB) --iters=40 \
	    | ./plot.py --snapshot $(SNAPSHOT_DIR)/multi_object.png \
	                --window-s 120 \
	                --title "bench/multi_object  heap + stack + tmpfile"
	@echo "wrote $(SNAPSHOT_DIR)/{linear,random,pointer_chase,multi_object}.png"

clean:
	rm -f $(BIN) $(BENCHES)
