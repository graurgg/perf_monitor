/*
 * linear.c - micro-benchmark with a controllable, easy-to-recognise memory
 * access pattern. The tracer's plot.py should visualise the heap region
 * filling bucket-by-bucket left-to-right when this is run with --mode=read
 * or --mode=write, since both walks are strictly sequential.
 *
 * Modes:
 *   --mode=read    sequential reads of every cache line
 *   --mode=write   sequential writes of every cache line
 *   --mode=rw      alternating reads and writes
 *
 * The default 64 MiB working set is large enough to dwarf L2 and exercise
 * the LSU pipeline, so IBS Op samples land on real load/store µops rather
 * than on cache-resident replays.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CACHELINE 64

enum mode { MODE_READ, MODE_WRITE, MODE_RW };

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    size_t size_mb = 64;
    int iters = 10;
    enum mode mode = MODE_RW;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--size-mb=", 10)) {
            size_mb = (size_t)strtoul(argv[i] + 10, NULL, 0);
        } else if (!strcmp(argv[i], "--size-mb") && i + 1 < argc) {
            size_mb = (size_t)strtoul(argv[++i], NULL, 0);
        } else if (!strncmp(argv[i], "--iters=", 8)) {
            iters = atoi(argv[i] + 8);
        } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--mode=read"))   mode = MODE_READ;
        else if  (!strcmp(argv[i], "--mode=write"))  mode = MODE_WRITE;
        else if  (!strcmp(argv[i], "--mode=rw"))     mode = MODE_RW;
        else {
            fprintf(stderr,
                    "Usage: %s [--size-mb N] [--iters N] "
                    "[--mode=read|write|rw]\n", argv[0]);
            return 2;
        }
    }

    size_t bytes = size_mb * 1024UL * 1024UL;
    /* Use calloc so the buffer is faulted in lazily — first-touch happens
     * inside the timed loop, which produces a clean left-to-right fill on
     * the address histogram on the first iteration. */
    volatile uint8_t *buf = calloc(bytes, 1);
    if (!buf) { perror("calloc"); return 1; }

    fprintf(stderr, "bench: size=%zu MiB iters=%d mode=%s\n",
            size_mb, iters,
            mode == MODE_READ ? "read" : mode == MODE_WRITE ? "write" : "rw");

    double t0 = now_s();
    uint64_t sink = 0;
    for (int it = 0; it < iters; it++) {
        switch (mode) {
        case MODE_READ:
            for (size_t i = 0; i < bytes; i += CACHELINE) sink += buf[i];
            break;
        case MODE_WRITE:
            for (size_t i = 0; i < bytes; i += CACHELINE)
                buf[i] = (uint8_t)(i ^ it);
            break;
        case MODE_RW:
            for (size_t i = 0; i < bytes; i += CACHELINE) {
                sink += buf[i];
                buf[i] = (uint8_t)(i ^ it);
            }
            break;
        }
    }
    double dt = now_s() - t0;

    /* Touch sink so the compiler doesn't elide the read path. */
    fprintf(stderr, "bench: %.3fs, sink=%" PRIu64 "\n", dt, sink);
    free((void *)buf);
    return 0;
}
