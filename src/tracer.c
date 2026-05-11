/*
 * tracer.c - Memory-access tracer based on AMD IBS Op sampling.
 *
 * Forks the target program, opens an ibs_op perf event on its PID with a
 * mmap'd ring buffer, and streams enriched (op, ip, ip_obj, addr,
 * data_obj, time, tid) records to stdout. PERF_RECORD_MMAP2 and
 * PERF_RECORD_COMM are also consumed from the ring buffer so that every
 * sampled IP / addr can be resolved to a memory-mapped object (a shared
 * library, the executable, the heap, the stack, or an anonymous /
 * JIT-style mapping) together with the in-object offset. Load/store
 * classification comes from PERF_SAMPLE_DATA_SRC
 * (perf_mem_data_src.mem_op bits) — a hardware classification supplied
 * by the IBS OP DATA3 register, not a software heuristic.
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

/* Maximum stored basename for a mapped object. Filenames longer than
 * this are truncated; full paths aren't useful for the plot anyway. */
#define MAX_OBJ_NAME 96

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

/* ----------------------------------------------------------------- *
 * Memory-map table.                                                  *
 *                                                                    *
 * The child's address space is described by a sorted-by-start array  *
 * of [start, end) intervals tagged with the basename of the backing  *
 * file (or a bracketed pseudo-name for [heap]/[stack]/[anon]). The   *
 * table is populated from PERF_RECORD_MMAP2 records that the kernel  *
 * emits for every mmap() the child performs (including the implicit  *
 * ones executed by the dynamic linker right after exec()).           *
 *                                                                    *
 * Overlapping inserts (mremap, munmap-then-mmap, the loader laying   *
 * down a library on top of a reserved region) drop the overlapping   *
 * portions of older entries. We don't bother coalescing — modern     *
 * processes have at most a few hundred VMAs, so binary-search lookup *
 * stays cheap.                                                       *
 * ----------------------------------------------------------------- */

struct map_entry {
    uint64_t start;     /* inclusive */
    uint64_t end;       /* exclusive */
    uint64_t pgoff;     /* byte offset of `start` inside the backing object */
    char     name[MAX_OBJ_NAME];
};

struct map_table {
    struct map_entry *entries;
    size_t n, cap;
};

static void map_init(struct map_table *t) {
    t->entries = NULL; t->n = 0; t->cap = 0;
}

static void map_reserve(struct map_table *t, size_t need) {
    if (t->cap >= need) return;
    size_t new_cap = t->cap ? t->cap * 2 : 64;
    while (new_cap < need) new_cap *= 2;
    struct map_entry *p = realloc(t->entries, new_cap * sizeof(*p));
    if (!p) die("realloc map_table: %s\n", strerror(errno));
    t->entries = p; t->cap = new_cap;
}

/* Pick a short, space-free display name. The kernel sends pseudo
 * mappings as "[heap]", "[stack]", "[vdso]"; we keep those. For real
 * files we drop the directory prefix. For nameless private/anon
 * mappings (kernel filename is "" or "//anon"), we tag them "[anon]"
 * — those are the JIT / large-malloc backing regions. */
static void shorten_name(const char *in, char *out, size_t outsz) {
    if (!in || !*in || strcmp(in, "//anon") == 0) {
        snprintf(out, outsz, "[anon]");
        return;
    }
    if (in[0] == '[') {                 /* [heap], [stack], [vdso], ... */
        snprintf(out, outsz, "%s", in);
        return;
    }
    const char *slash = strrchr(in, '/');
    snprintf(out, outsz, "%s", slash ? slash + 1 : in);
    /* Defensive: scrub whitespace so the output stays one record per
     * line and parseable by plot.py's whitespace split. */
    for (char *p = out; *p; p++) if (*p == ' ' || *p == '\t') *p = '_';
}

/* Trim every existing entry that overlaps [s, e). For simplicity we
 * just remove any entry that intersects; we don't split. The very next
 * MMAP2 in the stream (or the absence of one) determines what gets
 * remapped on top. */
static void map_remove_overlap(struct map_table *t, uint64_t s, uint64_t e) {
    size_t w = 0;
    for (size_t i = 0; i < t->n; i++) {
        struct map_entry *m = &t->entries[i];
        int overlaps = !(m->end <= s || m->start >= e);
        if (!overlaps) {
            if (w != i) t->entries[w] = *m;
            w++;
        }
    }
    t->n = w;
}

static int cmp_entry(const void *a, const void *b) {
    uint64_t sa = ((const struct map_entry *)a)->start;
    uint64_t sb = ((const struct map_entry *)b)->start;
    return (sa < sb) ? -1 : (sa > sb);
}

static void map_insert(struct map_table *t, uint64_t start, uint64_t len,
                       uint64_t pgoff, const char *filename, uint64_t time_ns) {
    if (len == 0) return;
    uint64_t end = start + len;
    map_remove_overlap(t, start, end);
    map_reserve(t, t->n + 1);
    struct map_entry *e = &t->entries[t->n++];
    e->start = start; e->end = end; e->pgoff = pgoff;
    shorten_name(filename, e->name, sizeof(e->name));
    /* Snapshot the display name before qsort moves entries around —
     * `e` is invalidated by the sort, so reading `e->name` after the
     * sort would print whatever record happened to land at that slot
     * (in practice: always the most recent [anon] insertion). */
    char display_name[MAX_OBJ_NAME];
    memcpy(display_name, e->name, sizeof(display_name));
    qsort(t->entries, t->n, sizeof(*t->entries), cmp_entry);
    /* Forward to stdout so the plotter knows each object's true
     * address range. Same whitespace-split format as samples but
     * tagged with leading 'M'. Time is the sample-id timestamp of
     * this MMAP2 record so the plotter can correlate it with the
     * sample stream. */
    printf("M %" PRIu64 " 0x%" PRIx64 " 0x%" PRIx64 " %s 0x%" PRIx64 "\n",
           time_ns, start, end, display_name, pgoff);
}

/* Binary-search the table for the entry containing `addr`. NULL if
 * none — caller prints "?" as the object name. */
static const struct map_entry *map_lookup(const struct map_table *t,
                                          uint64_t addr) {
    size_t lo = 0, hi = t->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const struct map_entry *m = &t->entries[mid];
        if (addr < m->start)        hi = mid;
        else if (addr >= m->end)    lo = mid + 1;
        else                        return m;
    }
    return NULL;
}

/* Format object + offset into `out`. "?" for unmapped — uniform width
 * so the CSV-ish stream stays easy to grep / awk. */
static void fmt_obj(const struct map_table *t, uint64_t addr,
                    char *out, size_t outsz) {
    const struct map_entry *m = map_lookup(t, addr);
    if (!m) { snprintf(out, outsz, "? 0x0"); return; }
    uint64_t off = (addr - m->start) + m->pgoff;
    snprintf(out, outsz, "%s 0x%" PRIx64, m->name, off);
}

/* ----------------------------------------------------------------- *
 * Ring-buffer drain.                                                 *
 * ----------------------------------------------------------------- */

/* Sample layout. Fields appear in the order their PERF_SAMPLE_* bits
 * are defined in <linux/perf_event.h>. Must match SAMPLE_TYPE below. */
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

/* Fixed-size part of a PERF_RECORD_MMAP2, right after perf_event_header.
 * Variable-length filename follows; sample_id (16B for TID+TIME) is
 * appended on the very end because attr.sample_id_all=1. */
struct mmap2_fixed {
    uint32_t pid, tid;
    uint64_t addr, len, pgoff;
    uint32_t maj, min;
    uint64_t ino, ino_generation;
    uint32_t prot, flags;
};

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
 * acquire), data_tail is written by us (store with release once
 * consumed). Records are processed strictly in ring order, which is the
 * order the kernel emitted them — that is what guarantees an MMAP2 for
 * a region lands in the table before any sample whose addr falls inside
 * that region is forwarded. */
static void drain(struct ringbuf *rb, struct map_table *maps,
                  uint64_t *lost_total) {
    uint64_t head = atomic_load_explicit(
        (atomic_uint_least64_t *)&rb->meta->data_head, memory_order_acquire);
    uint64_t tail = rb->meta->data_tail;

    while (tail < head) {
        struct perf_event_header hdr;
        rb_read(rb, tail, &hdr, sizeof(hdr));

        if (hdr.type == PERF_RECORD_SAMPLE) {
            struct sample s;
            rb_read(rb, tail + sizeof(hdr), &s, sizeof(s));
            /* Software substitute for exclude_kernel (which the AMD
             * IBS PMU rejects on this kernel): x86-64 kernel virtual
             * addresses have the top bit set. Drop any sample whose
             * ip OR addr lies in kernel space — IBS can occasionally
             * tag a kernel-side data address against a userspace IP
             * when a syscall is in flight. */
            char op = classify_op(s.data_src);
            const uint64_t KMASK = 1ULL << 63;
            if (op && s.addr && !(s.ip & KMASK) && !(s.addr & KMASK)) {
                char ip_obj[160], data_obj[160];
                fmt_obj(maps, s.ip,   ip_obj,   sizeof(ip_obj));
                fmt_obj(maps, s.addr, data_obj, sizeof(data_obj));
                printf("%c %" PRIu64 " 0x%" PRIx64 " %s 0x%" PRIx64
                       " %s %u\n",
                       op, s.time, s.ip, ip_obj, s.addr, data_obj, s.tid);
            }
        } else if (hdr.type == PERF_RECORD_MMAP2) {
            struct mmap2_fixed m;
            rb_read(rb, tail + sizeof(hdr), &m, sizeof(m));
            /* Filename starts right after the fixed part and is
             * null-padded to u64. The trailing 16 bytes are the
             * sample_id appended because sample_id_all=1: TID (8B) +
             * TIME (8B). We need the time so the plotter can place
             * the MMAP record on the same clock as the samples. */
            size_t off = sizeof(hdr) + sizeof(m);
            size_t avail = (hdr.size > off + 16) ? hdr.size - off - 16 : 0;
            char fname[512];
            size_t take = avail < sizeof(fname) - 1 ? avail : sizeof(fname) - 1;
            rb_read(rb, tail + off, fname, take);
            fname[take] = '\0';
            uint64_t time_ns = 0;
            rb_read(rb, tail + hdr.size - 8, &time_ns, sizeof(time_ns));
            map_insert(maps, m.addr, m.len, m.pgoff, fname, time_ns);
        } else if (hdr.type == PERF_RECORD_MMAP) {
            /* Legacy MMAP (no maj/min/ino fields). Older form, but we
             * accept it to be safe — same useful fields. Layout: pid,
             * tid, addr, len, pgoff, filename[]. */
            struct { uint32_t pid, tid; uint64_t addr, len, pgoff; } m;
            rb_read(rb, tail + sizeof(hdr), &m, sizeof(m));
            size_t off = sizeof(hdr) + sizeof(m);
            size_t avail = (hdr.size > off + 16) ? hdr.size - off - 16 : 0;
            char fname[512];
            size_t take = avail < sizeof(fname) - 1 ? avail : sizeof(fname) - 1;
            rb_read(rb, tail + off, fname, take);
            fname[take] = '\0';
            uint64_t time_ns = 0;
            rb_read(rb, tail + hdr.size - 8, &time_ns, sizeof(time_ns));
            map_insert(maps, m.addr, m.len, m.pgoff, fname, time_ns);
        } else if (hdr.type == PERF_RECORD_LOST) {
            struct { uint64_t id, lost; } lost;
            rb_read(rb, tail + sizeof(hdr), &lost, sizeof(lost));
            *lost_total += lost.lost;
        }
        /* COMM / FORK / EXIT / THROTTLE: not load-bearing for the
         * plot. We keep COMM-tracking enabled because some kernels
         * elide MMAP2 records until COMM has been issued for the new
         * exec()'d task. */

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
    /* Note: exclude_kernel / exclude_hv are NOT set. On this kernel the
     * AMD IBS PMU rejects perf_event_open with EINVAL when either is
     * requested (no swfilt fallback in practice). We filter kernel-mode
     * samples in software in drain(). */
    attr.enable_on_exec = 1;
    attr.wakeup_events  = 64;
    attr.sample_id_all  = 1;
    /* Track address-space changes so we can resolve every sampled
     * ip/addr to a mapped object (task 2). mmap2 carries the prot
     * bits and inode that mmap doesn't; mmap_data covers
     * non-executable mappings (heap, anon, file data) which a
     * "code-only" perf record would skip. comm/task let us follow
     * thread spawn/exit so multi-threaded targets work too. */
    attr.mmap           = 1;
    attr.mmap2          = 1;
    attr.mmap_data      = 1;
    attr.comm           = 1;
    attr.task           = 1;
    /* NOTE: attr.inherit is intentionally NOT set. AMD IBS is a
     * per-CPU PMU, and the kernel refuses mmap() on the perf fd
     * (EINVAL) when an inheritable per-task IBS event is requested
     * — there is no single ring buffer that can be shared across
     * inherited tasks. The practical consequence is that for a
     * multi-threaded target we only sample the initial thread.
     * Our benchmarks are single-threaded, so this is fine. */

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

    struct map_table maps;
    map_init(&maps);

    /* Header row makes plot.py's job easier and lets a human eyeball
     * the stream with `tee`. */
    printf("# op time ip ip_obj ip_off addr data_obj data_off tid\n");
    fflush(stdout);

    /* Release child to exec(); enable_on_exec=1 arms the counter at
     * exec time, so the loader and our own image aren't sampled. The
     * kernel emits MMAP2 records as the loader maps libraries — they
     * sit in the ring buffer ahead of any sample that hits the new
     * region, so map_lookup() at sample time will resolve. */
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
        if (r > 0) drain(&rb, &maps, &lost_total);

        int status;
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w == child) child_done = 1;
        else if (w < 0 && errno != EINTR) child_done = 1;
    }

    drain(&rb, &maps, &lost_total);

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    munmap(map, map_size);
    close(fd);
    free(maps.entries);

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
