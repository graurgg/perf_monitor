/*
 * tracer.c - Memory-access tracer based on AMD IBS Op sampling.
 *
 * Forks the target program, opens an ibs_op perf event on its PID with a
 * mmap'd ring buffer, and streams (op, ip, addr, time, tid) records to stdout
 * as the kernel delivers PERF_RECORD_SAMPLE entries. Load/store classification
 * comes from PERF_SAMPLE_DATA_SRC (perf_mem_data_src.mem_op bits).
 *
 * Intended pairing: ./tracer ... -- <cmd> | ./plot.py
 *
 * Build: see Makefile. Requires native Linux on an AMD CPU with IBS
 * (Family 10h or newer). Not visible inside WSL2 or most cloud VMs —
 * the host's hypervisor must expose ibs_op.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Number of ring-buffer data pages (must be a power of two). 256 pages =
 * 1 MiB of payload on 4 KiB-page systems. Bigger = fewer LOST records but
 * more RSS. */
#define DATA_PAGES 256

/* Dispatched ops between samples. The default leaves the bench busy
 * without flooding stdout; tune with -p. */
#define DEFAULT_PERIOD 10000

/* Bit 19 of the IBS Op config selects "count dispatched ops" rather than
 * cycles. Confirmed against arch/x86/events/amd/ibs.c:
 *     PMU_FORMAT_ATTR(cnt_ctl, "config:19"); */
#define IBS_OP_CNT_CTL (1ULL << 19)

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                            int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("tracer: ", stderr);
    vfprintf(stderr, fmt, ap);
    if (fmt[strlen(fmt) - 1] != '\n') fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static int read_ibs_op_type(void) {
    FILE *f = fopen("/sys/bus/event_source/devices/ibs_op/type", "r");
    if (!f) return -1;
    int type = -1;
    if (fscanf(f, "%d", &type) != 1) type = -1;
    fclose(f);
    return type;
}

/* Sample layout. Fields appear in the order their PERF_SAMPLE_* bits are
 * defined in <linux/perf_event.h>. Must match SAMPLE_TYPE below. */
struct sample {
    uint64_t ip;
    uint32_t pid;
    uint32_t tid;
    uint64_t time;
    uint64_t addr;
    uint64_t data_src;
};

static const uint64_t SAMPLE_TYPE = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
                                    PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR |
                                    PERF_SAMPLE_DATA_SRC;

/* Decode the mem_op field of perf_mem_data_src. Returns 'R', 'W', or 0
 * for ops that are neither (some prefetches). The kernel fills data_src
 * from IBS_OP_DATA3 for AMD — this is a hardware classification, not a
 * heuristic. */
static char classify_op(uint64_t data_src) {
    union perf_mem_data_src d;
    d.val = data_src;
    if (d.mem_op & PERF_MEM_OP_LOAD)  return 'R';
    if (d.mem_op & PERF_MEM_OP_STORE) return 'W';
    return 0;
}

struct ringbuf {
    struct perf_event_mmap_page *meta;
    uint8_t *data;
    size_t   data_size;
};

static void rb_read(struct ringbuf *rb, uint64_t off, void *dst, size_t len) {
    size_t start = off % rb->data_size;
    size_t first = rb->data_size - start;
    if (first >= len) {
        memcpy(dst, rb->data + start, len);
    } else {
        memcpy(dst, rb->data + start, first);
        memcpy((uint8_t *)dst + first, rb->data, len - first);
    }
}

static volatile sig_atomic_t got_sigint = 0;
static void on_sigint(int sig) { (void)sig; got_sigint = 1; }

/* Drain the ring buffer. data_head is written by the kernel (load with
 * acquire), data_tail is written by us (store with release once consumed). */
static void drain(struct ringbuf *rb, uint64_t *lost_total) {
    uint64_t head = atomic_load_explicit(
        (atomic_uint_least64_t *)&rb->meta->data_head, memory_order_acquire);
    uint64_t tail = rb->meta->data_tail;

    while (tail < head) {
        struct perf_event_header hdr;
        rb_read(rb, tail, &hdr, sizeof(hdr));

        if (hdr.type == PERF_RECORD_SAMPLE) {
            struct sample s;
            rb_read(rb, tail + sizeof(hdr), &s, sizeof(s));
            char op = classify_op(s.data_src);
            if (op && s.addr) {
                printf("%c %" PRIu64 " 0x%" PRIx64 " 0x%" PRIx64 " %u\n",
                       op, s.time, s.ip, s.addr, s.tid);
            }
        } else if (hdr.type == PERF_RECORD_LOST) {
            struct { uint64_t id, lost; } lost;
            rb_read(rb, tail + sizeof(hdr), &lost, sizeof(lost));
            *lost_total += lost.lost;
        }
        /* Other record types (MMAP, COMM, EXIT, THROTTLE...) are not
         * relevant to tasks 1 and 3 and are ignored. */

        tail += hdr.size;
    }
    atomic_store_explicit(
        (atomic_uint_least64_t *)&rb->meta->data_tail, head,
        memory_order_release);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [-p PERIOD] [-c CPU] -- CMD [ARGS...]\n"
            "  -p PERIOD  IBS sample period in dispatched ops (default %d)\n"
            "  -c CPU     Restrict tracing to a specific CPU (default -1)\n",
            argv0, DEFAULT_PERIOD);
    exit(2);
}

int main(int argc, char **argv) {
    long period = DEFAULT_PERIOD;
    int cpu = -1;

    int sep = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { sep = i; break; }
    }
    if (sep < 0 || sep == argc - 1) usage(argv[0]);

    for (int i = 1; i < sep; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < sep) {
            period = strtol(argv[++i], NULL, 0);
            if (period <= 0) die("period must be positive\n");
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < sep) {
            cpu = atoi(argv[++i]);
        } else {
            usage(argv[0]);
        }
    }
    char **child_argv = &argv[sep + 1];

    int ibs_type = read_ibs_op_type();
    if (ibs_type < 0) {
        die("ibs_op PMU not available. This requires native Linux on an "
            "AMD CPU with IBS — not exposed in WSL2 or most cloud VMs.\n");
    }

    /* Sync pipe: child blocks until parent has finished setting up the
     * perf event, then closes the write end to release exec(). */
    int sync_pipe[2];
    if (pipe(sync_pipe) < 0) die("pipe: %s\n", strerror(errno));

    pid_t child = fork();
    if (child < 0) die("fork: %s\n", strerror(errno));
    if (child == 0) {
        close(sync_pipe[1]);
        char b;
        if (read(sync_pipe[0], &b, 1) < 0) _exit(127);
        close(sync_pipe[0]);
        execvp(child_argv[0], child_argv);
        fprintf(stderr, "tracer: execvp(%s): %s\n",
                child_argv[0], strerror(errno));
        _exit(127);
    }
    close(sync_pipe[0]);

    struct perf_event_attr attr = {0};
    attr.type           = (uint32_t)ibs_type;
    attr.size           = sizeof(attr);
    attr.config         = IBS_OP_CNT_CTL;
    attr.sample_period  = (uint64_t)period;
    attr.sample_type    = SAMPLE_TYPE;
    attr.disabled       = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv     = 1;
    attr.enable_on_exec = 1;
    attr.wakeup_events  = 64;
    attr.sample_id_all  = 1;

    int fd = (int)perf_event_open(&attr, child, cpu, -1, 0);
    if (fd < 0) {
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        die("perf_event_open: %s. Check perf_event_paranoid (need <=2 or "
            "CAP_PERFMON) and that you are on bare-metal Linux.\n",
            strerror(errno));
    }

    long page = sysconf(_SC_PAGESIZE);
    size_t data_size = (size_t)page * DATA_PAGES;
    size_t map_size  = (size_t)page + data_size;
    void *map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        kill(child, SIGKILL); waitpid(child, NULL, 0);
        die("mmap ringbuf: %s\n", strerror(errno));
    }
    struct ringbuf rb = {
        .meta = (struct perf_event_mmap_page *)map,
        .data = (uint8_t *)map + page,
        .data_size = data_size,
    };

    /* Header row makes plot.py's job easier and lets a human eyeball
     * the stream with `tee`. */
    printf("# op time ip addr tid\n");
    fflush(stdout);

    /* Release child to exec(); enable_on_exec=1 arms the counter at
     * exec time, so the loader and our own image aren't sampled. */
    close(sync_pipe[1]);

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint64_t lost_total = 0;
    int child_done = 0;

    while (!child_done && !got_sigint) {
        int r = poll(&pfd, 1, 100);
        if (r < 0 && errno != EINTR) {
            fprintf(stderr, "tracer: poll: %s\n", strerror(errno));
            break;
        }
        if (r > 0) drain(&rb, &lost_total);

        int status;
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w == child) child_done = 1;
        else if (w < 0 && errno != EINTR) child_done = 1;
    }

    drain(&rb, &lost_total);

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    munmap(map, map_size);
    close(fd);

    if (got_sigint) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }

    if (lost_total)
        fprintf(stderr, "tracer: %" PRIu64 " samples lost (ring buffer "
                        "overrun — raise -p or increase DATA_PAGES)\n",
                lost_total);
    return 0;
}
