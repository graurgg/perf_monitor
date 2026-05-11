#!/usr/bin/env python3
"""
plot.py - live matplotlib visualisation of tracer output.

Consumes the tracer's enriched record stream from stdin:

    # header line (ignored)
    M <time_ns> 0x<start> 0x<end> <obj> 0x<pgoff>
    R <time_ns> 0x<ip>    <ip_obj> 0x<ip_off> 0x<addr> <data_obj> 0x<data_off> <tid>
    W <time_ns> 0x<ip>    <ip_obj> 0x<ip_off> 0x<addr> <data_obj> 0x<data_off> <tid>

Four panels are updated at ~5 Hz:

  (top, wide) Rate plot:  reads/second and writes/second over the last
              WINDOW_S seconds (default 60), bucketed into 1-second bins.
              Sample time is the kernel-recorded sample timestamp, not
              the time we consumed it — so a delayed plotter doesn't
              shift the curves.

  (mid-left)  Code-object breakdown: horizontal bar chart of R+W counts
              per IP-resolved object (libc, the executable, the loader,
              ...), restricted to the time window.

  (mid-right) Data-object breakdown: same for the address side
              (the 32 MiB anon buffer, [heap], [stack], libc data
              segments, ...).

  (bottom)    Intra-object address histogram for the busiest data
              object in the window, bucketed into HIST_BINS bins across
              the object's true mapped range (from the M records).
              For a sequential workload like bench/linear, this is
              where you watch the buckets fill bucket-by-bucket from
              low to high.

The CLI flag --window-s overrides WINDOW_S (the task's "limit displayed
samples to a user-specified time window" bonus).

Usage:
    ./tracer ... -- <cmd> | ./plot.py [--window-s 60] [--bins 64]

Snapshot mode (no GUI — for docs):

    ./tracer ... -- <cmd> | ./plot.py --snapshot screenshots/linear.png

Reads stdin to EOF, renders one full-window frame, writes a PNG, exits.
The window-s flag still applies: the frame reflects the last N seconds.
"""

import argparse
import collections
import sys
import threading

import matplotlib

# Backend selection is deferred until we know whether we're in snapshot
# mode — TkAgg is right for the live window, Agg for headless PNG.
_BACKEND_CHOSEN = False
def _select_backend(snapshot: bool) -> None:
    global _BACKEND_CHOSEN
    if _BACKEND_CHOSEN:
        return
    try:
        matplotlib.use("Agg" if snapshot else "TkAgg")
    except Exception:
        pass
    _BACKEND_CHOSEN = True

import matplotlib.pyplot as plt
import matplotlib.animation as animation
import matplotlib.gridspec as gridspec
import numpy as np


WINDOW_SAMPLES = 400_000   # cap for the in-memory rolling buffer
HIST_BINS = 64
FRAME_MS = 200
TOP_N_OBJECTS = 8          # bars per side in the breakdown panels


class Mappings:
    """All M records, with per-name contiguous-segment grouping.

    A naive "union by name" approach breaks for `[anon]` — the kernel
    uses that name for both the 32 MiB buffer (our actual workload) and
    a 4 KiB stack guard page near the top of userspace. The union of
    the two spans 16 TiB and gives a useless histogram.

    Instead we store every M record verbatim and, on demand, group
    contiguous (abutting / overlapping) entries with the same name into
    segments. Each library's .text / .rodata / .data become one
    segment; the 32 MiB anon buffer is its own segment, separate from
    the stack-guard anon page. Histogram bucketing then targets the
    single busiest segment for the busiest data-object name.
    """

    def __init__(self) -> None:
        self._maps: list[tuple[int, int, str]] = []  # (start, end, name)
        self._lock = threading.Lock()

    def add(self, name: str, start: int, end: int) -> None:
        with self._lock:
            self._maps.append((start, end, name))

    def segments_for(self, name: str) -> list[tuple[int, int]]:
        with self._lock:
            ranges = sorted([(s, e) for s, e, n in self._maps if n == name])
        if not ranges:
            return []
        out = [ranges[0]]
        for s, e in ranges[1:]:
            ls, le = out[-1]
            if s <= le:                     # contiguous or overlapping
                out[-1] = (ls, max(le, e))
            else:
                out.append((s, e))
        return out


class SampleBuffer:
    """Thread-safe rolling window of sample tuples."""

    def __init__(self, maxlen: int) -> None:
        self._buf = collections.deque(maxlen=maxlen)
        self._lock = threading.Lock()
        self.total_r = 0
        self.total_w = 0

    def push(self, op: str, t_s: float, ip_obj: str, addr: int,
             data_obj: str) -> None:
        with self._lock:
            self._buf.append((op, t_s, ip_obj, addr, data_obj))
            if op == "R":
                self.total_r += 1
            else:
                self.total_w += 1

    def snapshot(self) -> list:
        with self._lock:
            return list(self._buf)


def reader_thread(buf: SampleBuffer, mappings: Mappings) -> None:
    """Parse tracer stdout line by line until EOF.

    The tracer's record set is small (R, W, M, comments). Everything
    that doesn't look like one of those is silently dropped — keeps
    the plotter robust against the tracer's stderr accidentally being
    interleaved (e.g. the "samples lost" notice at EOF).
    """
    for line in sys.stdin:
        if not line or line[0] == "#":
            continue
        parts = line.split()
        if not parts:
            continue
        tag = parts[0]

        if tag == "M" and len(parts) >= 6:
            try:
                start = int(parts[2], 16)
                end   = int(parts[3], 16)
                name  = parts[4]
            except ValueError:
                continue
            mappings.add(name, start, end)
            continue

        if tag in ("R", "W") and len(parts) >= 9:
            try:
                t_ns = int(parts[1])
                ip_obj = parts[3]
                addr = int(parts[5], 16)
                data_obj = parts[6]
            except ValueError:
                continue
            if addr == 0:
                continue
            buf.push(tag, t_ns / 1e9, ip_obj, addr, data_obj)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--window-s", type=float, default=60.0,
                   help="time window (seconds) for rate + per-object counts")
    p.add_argument("--bins", type=int, default=HIST_BINS,
                   help="bins for the intra-object address histogram")
    p.add_argument("--snapshot", metavar="PATH",
                   help="non-interactive: read stdin to EOF, render one "
                        "frame, save a PNG to PATH, and exit (for docs).")
    p.add_argument("--title", default=None,
                   help="optional suptitle for the figure (snapshot mode).")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    _select_backend(snapshot=bool(args.snapshot))

    buf = SampleBuffer(WINDOW_SAMPLES)
    mappings = Mappings()
    # In snapshot mode we need to JOIN the reader before rendering, so
    # the thread can't be a daemon — it has to keep running until
    # stdin closes. In live mode we want daemon=True so Ctrl-C from
    # the plot window terminates immediately.
    reader = threading.Thread(target=reader_thread, args=(buf, mappings),
                              daemon=not args.snapshot)
    reader.start()

    fig = plt.figure(figsize=(12, 9))
    if not args.snapshot:
        fig.canvas.manager.set_window_title("perf_monitor — live")
    if args.title:
        fig.suptitle(args.title, fontsize=12, fontweight="bold")
    gs = gridspec.GridSpec(3, 2, figure=fig,
                           height_ratios=[1.0, 1.1, 1.3], hspace=0.45,
                           wspace=0.25)
    ax_rate   = fig.add_subplot(gs[0, :])
    ax_ipbar  = fig.add_subplot(gs[1, 0])
    ax_dbar   = fig.add_subplot(gs[1, 1])
    ax_hist   = fig.add_subplot(gs[2, :])

    ax_rate.set_xlabel(f"time relative to first sample (s); window={args.window_s:g}s")
    ax_rate.set_ylabel("samples / second")
    ax_rate.set_title("Memory-access rate (R vs W)")
    (line_r,) = ax_rate.plot([], [], label="reads",  color="tab:blue")
    (line_w,) = ax_rate.plot([], [], label="writes", color="tab:red")
    ax_rate.legend(loc="upper left")
    ax_rate.grid(True, alpha=0.3)

    text_total = fig.text(
        0.99, 0.985, "", ha="right", va="top", family="monospace", fontsize=9
    )

    t_origin = [None]  # mutable cell, set on first sample

    def draw_obj_bars(ax, counts_r: dict, counts_w: dict, title: str,
                      empty_msg: str) -> None:
        ax.clear()
        ax.set_title(title)
        ax.set_xlabel("samples in window")
        names = sorted(set(counts_r) | set(counts_w),
                       key=lambda n: -(counts_r.get(n, 0) + counts_w.get(n, 0)))
        names = names[:TOP_N_OBJECTS]
        if not names:
            ax.text(0.5, 0.5, empty_msg, ha="center", va="center",
                    transform=ax.transAxes, color="gray")
            return
        y = np.arange(len(names))
        rs = np.array([counts_r.get(n, 0) for n in names], dtype=float)
        ws = np.array([counts_w.get(n, 0) for n in names], dtype=float)
        ax.barh(y, rs, color="tab:blue", label="reads")
        ax.barh(y, ws, left=rs, color="tab:red", label="writes")
        ax.set_yticks(y)
        ax.set_yticklabels(names, fontsize=8)
        ax.invert_yaxis()
        ax.legend(loc="lower right", fontsize=8)

    def draw_intra_object(ax, snap: list, lo_rel: float, t0: float) -> None:
        ax.clear()
        # Pick the busiest data object in the time window.
        counts_d = collections.Counter()
        for op, ts, _ipo, _addr, dobj in snap:
            if ts - t0 >= lo_rel:
                counts_d[dobj] += 1
        if not counts_d:
            ax.text(0.5, 0.5, "(no samples yet)", ha="center", va="center",
                    transform=ax.transAxes, color="gray")
            ax.set_title("Top data object — within-object access histogram")
            return
        top_obj, _ = counts_d.most_common(1)[0]
        # Gather in-window samples for the busiest object name.
        addrs_r, addrs_w = [], []
        for op, ts, _ipo, addr, dobj in snap:
            if dobj != top_obj or ts - t0 < lo_rel:
                continue
            (addrs_r if op == "R" else addrs_w).append(addr)
        if not addrs_r and not addrs_w:
            ax.text(0.5, 0.5, "(no samples)", ha="center", va="center",
                    transform=ax.transAxes, color="gray")
            return
        # Pick the busiest *contiguous segment* of mappings with that
        # name, then bucket within it. For libc.so.6 with 5 abutting
        # ELF segments this is the union; for [anon] with one big calloc
        # buffer + a small stack-guard page this picks the big buffer.
        segments = mappings.segments_for(top_obj)
        seg_counts = [0] * max(1, len(segments))
        all_addrs = addrs_r + addrs_w
        for a in all_addrs:
            for i, (s, e) in enumerate(segments):
                if s <= a < e:
                    seg_counts[i] += 1
                    break
        if segments and max(seg_counts) > 0:
            best_idx = seg_counts.index(max(seg_counts))
            lo_addr, hi_addr = segments[best_idx]
            addrs_r = [a for a in addrs_r if lo_addr <= a < hi_addr]
            addrs_w = [a for a in addrs_w if lo_addr <= a < hi_addr]
        else:
            # No M-record info yet — fall back to observed range.
            lo_addr, hi_addr = min(all_addrs), max(all_addrs) + 1
        if hi_addr <= lo_addr:
            hi_addr = lo_addr + 1
        edges = np.linspace(lo_addr, hi_addr, args.bins + 1)
        ax.hist(addrs_r, bins=edges, alpha=0.6, color="tab:blue", label="reads")
        ax.hist(addrs_w, bins=edges, alpha=0.6, color="tab:red",  label="writes")
        ax.legend(loc="upper right", fontsize=8)
        size_mib = (hi_addr - lo_addr) / (1024 * 1024)
        ax.set_title(f"Within-object access histogram — {top_obj}  "
                     f"({size_mib:.1f} MiB across {args.bins} bins)")
        ax.set_xlabel("virtual address")
        ax.set_ylabel("samples in window")
        ax.ticklabel_format(axis="x", style="sci", scilimits=(0, 0))

    def update(_frame):
        snap = buf.snapshot()
        if not snap:
            return ()

        if t_origin[0] is None:
            t_origin[0] = snap[0][1]
        t0 = t_origin[0]
        now_rel = snap[-1][1] - t0
        lo_rel = max(0.0, now_rel - args.window_s)

        # --- Rate plot: 1s bins over the window.
        times_r = []
        times_w = []
        cnt_ip_r: dict = collections.Counter()
        cnt_ip_w: dict = collections.Counter()
        cnt_d_r:  dict = collections.Counter()
        cnt_d_w:  dict = collections.Counter()
        for op, ts, ipo, _addr, dobj in snap:
            rel = ts - t0
            if rel < lo_rel:
                continue
            if op == "R":
                times_r.append(rel); cnt_ip_r[ipo] += 1; cnt_d_r[dobj] += 1
            else:
                times_w.append(rel); cnt_ip_w[ipo] += 1; cnt_d_w[dobj] += 1

        n_bins = max(1, int(now_rel - lo_rel) + 1)
        edges = np.linspace(lo_rel, lo_rel + n_bins, n_bins + 1)
        hist_r, _ = np.histogram(times_r, bins=edges)
        hist_w, _ = np.histogram(times_w, bins=edges)
        centers = (edges[:-1] + edges[1:]) / 2.0
        line_r.set_data(centers, hist_r)
        line_w.set_data(centers, hist_w)
        ax_rate.set_xlim(lo_rel, lo_rel + n_bins)
        ymax = max(hist_r.max() if len(hist_r) else 1,
                   hist_w.max() if len(hist_w) else 1, 1)
        ax_rate.set_ylim(0, ymax * 1.2)

        # --- Bar charts for code/data object breakdown.
        draw_obj_bars(ax_ipbar, cnt_ip_r, cnt_ip_w,
                      "R+W by code object (IP)", "(no samples in window)")
        draw_obj_bars(ax_dbar,  cnt_d_r,  cnt_d_w,
                      "R+W by data object (addr)", "(no samples in window)")

        # --- Intra-object histogram for the busiest data object.
        draw_intra_object(ax_hist, snap, lo_rel, t0)

        win_r = sum(cnt_d_r.values())
        win_w = sum(cnt_d_w.values())
        text_total.set_text(
            f"cumulative R={buf.total_r:>10d}  W={buf.total_w:>10d}\n"
            f"window     R={win_r:>10d}  W={win_w:>10d}  "
            f"({args.window_s:g}s)"
        )
        return ()

    if args.snapshot:
        # Block until the tracer's stdout closes, then render a single
        # frame from everything we received. window-s applies as
        # always — to get the *whole* run in one frame, pass a value
        # larger than the run duration (default 60s is usually fine).
        reader.join()
        update(0)
        fig.savefig(args.snapshot, dpi=110, bbox_inches="tight")
        print(f"plot.py: wrote {args.snapshot} "
              f"(R={buf.total_r}, W={buf.total_w})", file=sys.stderr)
        return 0

    ani = animation.FuncAnimation(
        fig, update, interval=FRAME_MS, blit=False, cache_frame_data=False
    )
    fig._ani = ani  # keep ref

    try:
        plt.show()
    except KeyboardInterrupt:
        pass

    # Best-effort drain so the tracer doesn't sit on a SIGPIPE.
    try:
        for _ in sys.stdin:
            pass
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
