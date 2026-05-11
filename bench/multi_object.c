/*
 * multi_object.c - exercises three distinct memory objects in a tight
 * interleaved loop. Purpose: demonstrate the address-to-object
 * resolution end of task 2 — the bar charts in plot.py should show
 * roughly even R+W counts across:
 *
 *   - a large heap-backed (calloc'd) buffer  →  [anon]
 *   - a stack-resident array                  →  [stack]
 *   - an mmap'd temp file                     →  the file's basename
 *
 * The driver loop touches one cache line in each object per iteration
 * so the three regions accumulate samples at the same rate. Watch the
 * "R+W by data object" bar in plot.py — three near-equal bars is the
 * visual signature.
 *
 * Usage:
 *   ./multi_object [--size-mb N] [--iters N]
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LINE 64

static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    size_t size_mb = 16;
    int iters = 30;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--size-mb=", 10))
            size_mb = (size_t)strtoul(argv[i] + 10, NULL, 0);
        else if (!strncmp(argv[i], "--iters=", 8))
            iters = atoi(argv[i] + 8);
    }
    size_t bytes = size_mb * 1024UL * 1024UL;

    /* (1) anon-mmap'd heap buffer */
    volatile uint8_t *heap = calloc(bytes, 1);
    if (!heap) { perror("calloc"); return 1; }

    /* (2) stack-resident array — big enough to be useful but well
     * below the default 8 MiB rlimit */
    enum { STACK_BYTES = 256 * 1024 };
    volatile uint8_t stack_arr[STACK_BYTES];
    memset((void *)stack_arr, 0, sizeof(stack_arr));

    /* (3) file-backed mmap */
    char path[] = "/tmp/multi_obj_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    /* Unlink immediately so it disappears on close. Linux still maps
     * it for the lifetime of the fd / mapping. The kernel reports the
     * pre-unlink path in PERF_RECORD_MMAP2 so the plot still has a
     * useful object name. */
    if (ftruncate(fd, (off_t)bytes) < 0) { perror("ftruncate"); return 1; }
    volatile uint8_t *file =
        mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (file == MAP_FAILED) { perror("mmap"); return 1; }
    unlink(path);

    fprintf(stderr, "bench: heap=%zu MiB stack=%zu KiB file=%zu MiB iters=%d (%s)\n",
            size_mb, sizeof(stack_arr) / 1024, size_mb, iters, path);

    /* Walk each region one cache line at a time, interleaved. The
     * stack region is smaller, so it wraps; that's fine — we just
     * want comparable sample counts across the three objects. */
    double t0 = now_s();
    uint64_t sink = 0;
    size_t lines = bytes / LINE;
    size_t stack_lines = STACK_BYTES / LINE;
    for (int it = 0; it < iters; it++) {
        for (size_t k = 0; k < lines; k++) {
            size_t a = k * LINE;
            sink += heap[a];                heap[a]              = (uint8_t)k;
            sink += stack_arr[(k % stack_lines) * LINE];
            stack_arr[(k % stack_lines) * LINE] = (uint8_t)k;
            sink += file[a];                file[a]              = (uint8_t)k;
        }
    }
    double dt = now_s() - t0;
    fprintf(stderr, "bench: %.3fs, sink=%" PRIu64 "\n", dt, sink);

    munmap((void *)file, bytes);
    close(fd);
    free((void *)heap);
    return 0;
}
