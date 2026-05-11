/*
 * pointer_chase.c - latency-bound, data-dependent memory access.
 *
 * Allocates an array of N cache-line-sized nodes, shuffles them into a
 * single random Hamiltonian cycle, and walks the cycle by following
 * each node's `next` pointer. The CPU can't prefetch — every load
 * must complete before the next address is known — so this is the
 * canonical pointer-chase workload, and it stresses the LSU much
 * harder than linear streaming.
 *
 * For task 3 this is interesting because:
 *   - The address histogram should look uniform across the buffer
 *     (the cycle visits every node exactly once per iteration).
 *   - The rate plot should sit lower than linear/random because each
 *     access stalls on a cache miss.
 *
 * Usage:
 *   ./pointer_chase [--size-mb N] [--iters N]
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LINE 64

struct node {
    struct node *next;
    uint8_t pad[LINE - sizeof(struct node *)];
};

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t xs_state = 0xDEADBEEFCAFEBABEULL;
static uint64_t xs(void) {
    uint64_t x = xs_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return xs_state = x;
}

int main(int argc, char **argv) {
    size_t size_mb = 64;
    int iters = 8;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--size-mb=", 10))
            size_mb = (size_t)strtoul(argv[i] + 10, NULL, 0);
        else if (!strcmp(argv[i], "--size-mb") && i + 1 < argc)
            size_mb = (size_t)strtoul(argv[++i], NULL, 0);
        else if (!strncmp(argv[i], "--iters=", 8))
            iters = atoi(argv[i] + 8);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = atoi(argv[++i]);
        else { fprintf(stderr, "Usage: %s [--size-mb N] [--iters N]\n", argv[0]); return 2; }
    }

    size_t bytes = size_mb * 1024UL * 1024UL;
    size_t n = bytes / sizeof(struct node);
    struct node *arr = aligned_alloc(LINE, n * sizeof(struct node));
    if (!arr) { perror("aligned_alloc"); return 1; }
    memset(arr, 0, n * sizeof(struct node));

    /* Permutation that produces a single cycle of length n: start
     * with arr[i].next = arr[(i+1)%n], then shuffle pairs. */
    for (size_t i = 0; i < n; i++) arr[i].next = &arr[(i + 1) % n];
    for (size_t i = n - 1; i > 1; i--) {
        size_t j = 1 + (size_t)(xs() % i);
        struct node *t = arr[i].next;
        arr[i].next = arr[j].next;
        arr[j].next = t;
    }
    /* The above isn't guaranteed to keep it a single cycle. Detect
     * and re-stitch any short cycles by following from arr[0] and
     * splicing in unvisited nodes. */
    char *seen = calloc(n, 1);
    if (!seen) { perror("calloc seen"); return 1; }
    struct node *p = arr;
    size_t visited = 0;
    while (visited < n) {
        size_t idx = (size_t)(p - arr);
        if (seen[idx]) {
            /* find an unvisited node and splice it after p */
            size_t u = 0;
            while (u < n && seen[u]) u++;
            if (u == n) break;
            arr[u].next = p->next;
            p->next = &arr[u];
        }
        seen[idx] = 1;
        visited++;
        p = p->next;
    }
    free(seen);

    fprintf(stderr, "bench: size=%zu MiB iters=%d nodes=%zu\n",
            size_mb, iters, n);

    double t0 = now_s();
    p = arr;
    for (int it = 0; it < iters; it++) {
        for (size_t i = 0; i < n; i++) p = p->next;
    }
    double dt = now_s() - t0;
    /* Touch the result so the loop isn't dead-code-eliminated. */
    fprintf(stderr, "bench: %.3fs, last=%p\n", dt, (void *)p);
    free(arr);
    return 0;
}
