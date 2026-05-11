#!/usr/bin/env python3
"""
plot.py - live matplotlib visualisation of tracer output.

Reads tracer records from stdin, one per line:

    # op time ip addr tid           <-- header (ignored)
    R <time_ns> 0x<ip> 0x<addr> <tid>
    W <time_ns> 0x<ip> 0x<addr> <tid>

Renders two panels updated ~5 Hz via FuncAnimation:

  (1) Reads/writes per second over wall-clock time (line plot, last 60 s).
  (2) Histogram of accessed addresses split by op (R/W as overlaid bars).

Address bucketing strategy: we don't try to map to mmap'd objects (that's
task 2, which the user is skipping). Instead we bucket the live address
range linearly into N bins. As new samples arrive outside the current
range, the bin edges are recomputed and the histogram is rebuilt from the
retained sample window. This is good enough to show "the heap region
fills up bucket by bucket" for the linear-access micro-benchmark.

Usage:
    ./tracer ... -- <cmd> | ./plot.py
    # or, for an offline pass:
    ./tracer ... -- <cmd> > samples.txt && ./plot.py < samples.txt
"""

import collections
import sys
import threading
import time

import matplotlib

# 'TkAgg' tends to be the most ubiquitously-present interactive backend on
# native Linux desktops; fall back to whatever matplotlib picks otherwise.
try:
    matplotlib.use("TkAgg")
except Exception:
    pass

import matplotlib.pyplot as plt
import matplotlib.animation as animation
import numpy as np


# Reader thread keeps a rolling window of recent samples. We deliberately
# cap memory rather than retain everything — for a long-running target the
# stream is unbounded. The plot only needs "recent" data.
WINDOW_SAMPLES = 200_000
HIST_BINS = 64

# Rate plot horizon in seconds.
RATE_HORIZON_S = 60.0

# Animation frame interval in milliseconds.
FRAME_MS = 200


class SampleBuffer:
    """Thread-safe rolling window of (op, time_s, ip, addr) tuples."""

    def __init__(self, maxlen: int):
        self._buf = collections.deque(maxlen=maxlen)
        self._lock = threading.Lock()
        # Cumulative counters since process start — never reset.
        self.total_r = 0
        self.total_w = 0

    def push(self, op: str, time_s: float, ip: int, addr: int) -> None:
        with self._lock:
            self._buf.append((op, time_s, ip, addr))
            if op == "R":
                self.total_r += 1
            else:
                self.total_w += 1

    def snapshot(self):
        with self._lock:
            return list(self._buf)


def reader_thread(buf: SampleBuffer) -> None:
    """Parse tracer stdout line by line until EOF."""
    for line in sys.stdin:
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) < 5:
            continue
        op = parts[0]
        if op not in ("R", "W"):
            continue
        try:
            t_ns = int(parts[1])
            ip = int(parts[2], 16)
            addr = int(parts[3], 16)
        except ValueError:
            continue
        # addr=0 happens for samples where IBS didn't tag a linear address
        # (e.g. some prefetch ops slipped past classify_op). Drop them — they
        # would dominate the lowest histogram bin and mislead the plot.
        if addr == 0:
            continue
        buf.push(op, t_ns / 1e9, ip, addr)


def main() -> int:
    buf = SampleBuffer(WINDOW_SAMPLES)
    t = threading.Thread(target=reader_thread, args=(buf,), daemon=True)
    t.start()

    fig, (ax_rate, ax_hist) = plt.subplots(2, 1, figsize=(10, 7))
    fig.canvas.manager.set_window_title("perf_monitor — live")

    ax_rate.set_xlabel("time relative to first sample (s)")
    ax_rate.set_ylabel("samples / second")
    ax_rate.set_title("Memory-access sample rate (R vs W)")
    (line_r,) = ax_rate.plot([], [], label="reads", color="tab:blue")
    (line_w,) = ax_rate.plot([], [], label="writes", color="tab:red")
    ax_rate.legend(loc="upper left")
    ax_rate.grid(True, alpha=0.3)

    ax_hist.set_xlabel("accessed virtual address")
    ax_hist.set_ylabel("samples (rolling window)")
    ax_hist.set_title("Accessed-address distribution")

    text_total = fig.text(
        0.99, 0.99, "", ha="right", va="top", family="monospace", fontsize=9
    )

    t_origin = [None]  # mutable cell, set on first sample

    def update(_frame):
        snap = buf.snapshot()
        if not snap:
            return line_r, line_w, text_total

        if t_origin[0] is None:
            t_origin[0] = snap[0][1]
        t0 = t_origin[0]

        # --- Rate plot: bucket the last RATE_HORIZON_S of samples into 1s bins.
        now_rel = snap[-1][1] - t0
        lo_rel = max(0.0, now_rel - RATE_HORIZON_S)
        # Filter (cheap: snap is ~200k worst case, runs at 5Hz).
        times_r = []
        times_w = []
        for op, ts, _ip, _addr in snap:
            rel = ts - t0
            if rel < lo_rel:
                continue
            (times_r if op == "R" else times_w).append(rel)

        # 1-second bins, integer-aligned for a stable look.
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

        # --- Address histogram across the entire retained window.
        addrs_r = np.fromiter((a for o, _t, _i, a in snap if o == "R"),
                              dtype=np.uint64)
        addrs_w = np.fromiter((a for o, _t, _i, a in snap if o == "W"),
                              dtype=np.uint64)
        all_addrs = np.concatenate([addrs_r, addrs_w]) if len(addrs_r) + len(addrs_w) else None

        ax_hist.clear()
        ax_hist.set_xlabel("accessed virtual address")
        ax_hist.set_ylabel("samples (rolling window)")
        ax_hist.set_title("Accessed-address distribution")
        if all_addrs is not None and len(all_addrs):
            lo = int(all_addrs.min())
            hi = int(all_addrs.max())
            if hi == lo:
                hi = lo + 1
            edges = np.linspace(lo, hi, HIST_BINS + 1)
            # Overlapping bars with transparency — visually shows when reads
            # and writes target the same region.
            ax_hist.hist(addrs_r, bins=edges, alpha=0.6,
                         label="reads", color="tab:blue")
            ax_hist.hist(addrs_w, bins=edges, alpha=0.6,
                         label="writes", color="tab:red")
            ax_hist.legend(loc="upper right")
            ax_hist.ticklabel_format(axis="x", style="sci", scilimits=(0, 0))

        text_total.set_text(
            f"total  R={buf.total_r:>10d}  W={buf.total_w:>10d}\n"
            f"window R={len(addrs_r):>10d}  W={len(addrs_w):>10d}"
        )

        return line_r, line_w, text_total

    # cache_frame_data=False: frames are live-derived; matplotlib otherwise
    # warns about an unbounded cache when the animation has no save_count.
    ani = animation.FuncAnimation(
        fig, update, interval=FRAME_MS, blit=False, cache_frame_data=False
    )
    # Keep a reference so it isn't GC'd.
    fig._ani = ani  # noqa: SLF001

    try:
        plt.show()
    except KeyboardInterrupt:
        pass

    # When the window closes, drain stdin so the upstream tracer doesn't
    # block on SIGPIPE — though tracer ignores SIGPIPE explicitly. Best
    # effort.
    try:
        for _ in sys.stdin:
            pass
    except Exception:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
