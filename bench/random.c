/*
 * random.c - random-access micro-benchmark.
 *
 * Allocates an anonymous buffer and touches it via a Fisher-Yates-
 * shuffled index array. Same cache-line stride as linear.c so the per-
 * sample cost is comparable, but the address histogram should look
 * close to uniform across the buffer's full range instead of the
 * left-to-right fill that linear.c produces. This is the visual
 * counter-example to linear.
 *
 * Usage:
 *   ./random [--size-mb N] [--iters N] [--mode read|write|rw]
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
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* xorshift64 — small, fast, deterministic. Plenty of randomness for
 * shuffling a few hundred thousand indices. */
static uint64_t xs_state = 0x9E3779B97F4A7C15ULL;
static uint64_t xs(void) {
    uint64_t x = xs_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return xs_state = x;
}

int main(int argc, char **argv) {
    size_t size_mb = 64;
    int iters = 5;
    enum mode mode = MODE_RW;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--size-mb=", 10))
            size_mb = (size_t)strtoul(argv[i] + 10, NULL, 0);
        else if (!strcmp(argv[i], "--size-mb") && i + 1 < argc)
            size_mb = (size_t)strtoul(argv[++i], NULL, 0);
        else if (!strncmp(argv[i], "--iters=", 8))
            iters = atoi(argv[i] + 8);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode=read"))  mode = MODE_READ;
        else if (!strcmp(argv[i], "--mode=write")) mode = MODE_WRITE;
        else if (!strcmp(argv[i], "--mode=rw"))    mode = MODE_RW;
        else {
            fprintf(stderr, "Usage: %s [--size-mb N] [--iters N] "
                    "[--mode=read|write|rw]\n", argv[0]);
            return 2;
        }
    }

    size_t bytes = size_mb * 1024UL * 1024UL;
    volatile uint8_t *buf = calloc(bytes, 1);
    if (!buf) { perror("calloc"); return 1; }

    size_t n_lines = bytes / CACHELINE;
    size_t *idx = malloc(n_lines * sizeof(*idx));
    if (!idx) { perror("malloc idx"); return 1; }
    for (size_t i = 0; i < n_lines; i++) idx[i] = i;
    /* Fisher-Yates. */
    for (size_t i = n_lines - 1; i > 0; i--) {
        size_t j = (size_t)(xs() % (i + 1));
        size_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }

    fprintf(stderr, "bench: size=%zu MiB iters=%d mode=%s n_lines=%zu\n",
            size_mb, iters,
            mode == MODE_READ ? "read" : mode == MODE_WRITE ? "write" : "rw",
            n_lines);

    double t0 = now_s();
    uint64_t sink = 0;
    for (int it = 0; it < iters; it++) {
        switch (mode) {
        case MODE_READ:
            for (size_t k = 0; k < n_lines; k++) sink += buf[idx[k] * CACHELINE];
            break;
        case MODE_WRITE:
            for (size_t k = 0; k < n_lines; k++)
                buf[idx[k] * CACHELINE] = (uint8_t)(k ^ it);
            break;
        case MODE_RW:
            for (size_t k = 0; k < n_lines; k++) {
                size_t a = idx[k] * CACHELINE;
                sink += buf[a];
                buf[a] = (uint8_t)(k ^ it);
            }
            break;
        }
    }
    double dt = now_s() - t0;
    fprintf(stderr, "bench: %.3fs, sink=%" PRIu64 "\n", dt, sink);
    free(idx); free((void *)buf);
    return 0;
}
