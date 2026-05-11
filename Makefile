CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wshadow -std=gnu11 -D_GNU_SOURCE
LDFLAGS ?=

BIN     := tracer
BENCH   := bench/linear

.PHONY: all bench clean run-bench

all: $(BIN) $(BENCH)

$(BIN): src/tracer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BENCH): bench/linear.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench: $(BENCH)

# Convenience target. Pipes the tracer through the plotter so a single
# `make run-bench` shows the live window. Override PERIOD or SIZE_MB on
# the command line, e.g. `make run-bench PERIOD=5000 SIZE_MB=256`.
PERIOD  ?= 10000
SIZE_MB ?= 64
run-bench: all
	./$(BIN) -p $(PERIOD) -- ./$(BENCH) --size-mb $(SIZE_MB) | ./plot.py

clean:
	rm -f $(BIN) $(BENCH)
