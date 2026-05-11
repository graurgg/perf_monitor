# perf_monitor

Linux perf-event-based tracer that monitors main memory accesses performed
by another process, plus a live matplotlib visualisation.

This implementation covers the assignment's tasks **1 (memory access
tracing)** and **3 (live plotting)**. Task 2 (mapping addresses to objects)
is intentionally out of scope.

## Hardware / kernel requirements

The tracer uses **AMD IBS Op sampling** as its source of memory-access
records. It requires:

- An AMD CPU with IBS — Family 10h (Barcelona) or newer. The Ryzen and
  EPYC families all qualify.
- A native Linux boot. WSL2 does **not** expose `ibs_op`, and most cloud
  / KVM guests don't either (the host has to enable PMU passthrough).
  Verify with:

      cat /sys/bus/event_source/devices/ibs_op/type   # should print an integer

- `kernel.perf_event_paranoid <= 2`:

      sudo sysctl kernel.perf_event_paranoid=2

- Python 3 with `matplotlib` and `numpy` for the plotter.

If `perf` is installed, a quick end-to-end check:

```bash
sudo perf record -e ibs_op// -c 10000 -d -- /bin/ls >/dev/null
sudo perf script | head
```

Lines should show non-zero accessed addresses.

## Running on a fresh Ubuntu live USB

The tracer needs bare-metal hardware access to the AMD IBS PMU, which no
hypervisor (WSL2, VirtualBox, most cloud KVM guests, Hyper-V) exposes.
The fastest way to get a working environment is to boot Ubuntu from a
USB stick in "Try Ubuntu" mode — no install required, nothing on the
host is touched. Ubuntu 24.04 LTS is the recommended image.

Once booted into the live session, open a terminal and run:

```bash
sudo apt update
sudo apt install -y build-essential git \
                    python3-matplotlib python3-numpy python3-tk
sudo sysctl kernel.perf_event_paranoid=2

# Sanity check — must print an integer:
cat /sys/bus/event_source/devices/ibs_op/type

git clone https://github.com/graurgg/perf_monitor.git
cd perf_monitor
make
./tracer -p 10000 -- ./bench/linear --size-mb=256 --iters=200 --mode=rw \
    | ./plot.py
```

A two-panel matplotlib window should appear and update live as the
benchmark walks its buffer. Capture screenshots before shutting down the
live session — nothing persists across reboot.

## Build

```bash
make
```

Produces `./tracer` and `./bench/linear`.

## Usage

```
./tracer [-p PERIOD] [-c CPU] -- CMD [ARGS...]
  -p PERIOD   IBS sample period in dispatched ops (default 10000).
              Lower = more samples / more overhead.
  -c CPU      Restrict tracing to a specific CPU (default -1 = all CPUs).
```

The tracer prints one CSV-ish line per sample to stdout:

```
# op time ip addr tid
R 1716304129834567890 0x7f5a1c4a9e30 0x55c000003040 12345
W 1716304129834579123 0x7f5a1c4a9e34 0x55c000003048 12345
```

- `op` is `R` (load) or `W` (store), decoded from
  `perf_mem_data_src.mem_op` — populated by the kernel from the IBS
  OP DATA3 register (hardware classification, not heuristic).
- `time` is the kernel-supplied sample timestamp in nanoseconds
  (`PERF_SAMPLE_TIME`).
- `ip` is the precise instruction pointer of the sampled µop.
- `addr` is the linear address it touched.
- `tid` is the kernel TID inside the traced process.

Samples whose `addr` is zero (rare: prefetch-classified ops) are dropped
by the plotter.

## Live plot

Pipe the tracer into `plot.py`:

```bash
./tracer -p 5000 -- ./bench/linear --size-mb=128 --mode=rw | ./plot.py
```

Two panels update at ~5 Hz:

1. **Rate plot** — reads/second and writes/second over the last 60 s,
   bucketed into 1-second bins.
2. **Address histogram** — the rolling window of recent sample addresses
   bucketed into 64 bins, overlaid R and W. For a sequential workload
   like `bench/linear`, you'll see the histogram fill bucket-by-bucket
   from low to high addresses as the benchmark walks its buffer.

A monospace overlay shows cumulative R / W counts since launch, and the
counts inside the current window.

There's also a one-shot convenience target:

```bash
make run-bench                           # 64 MiB, period=10000
make run-bench SIZE_MB=256 PERIOD=5000   # heavier
```

## Files

```
src/tracer.c     C tracer (perf_event_open + IBS Op + ring buffer reader)
plot.py          live matplotlib plotter, reads tracer stdout
bench/linear.c   sequential read/write/rw micro-benchmark
Makefile         build + run-bench convenience target
```

## Notes on soundness (for the writeup, if you wind up needing one)

- Sampling is uniform-in-ops because we set `cnt_ctl = 1` (IBS Op
  counter measures dispatched µops, not cycles). This avoids the
  artefact where cycle-based sampling would bias towards
  memory-stalled regions.
- The counter is armed via `enable_on_exec = 1` after a pipe-based
  fork-exec rendezvous, so no sample from the tracer's own image or
  from the dynamic loader pollutes the child's data.
- `PERF_RECORD_LOST` records are accumulated and reported on exit; a
  non-zero count means you should raise `-p` or increase `DATA_PAGES`
  in `tracer.c`.
- Reads vs writes come from `perf_mem_data_src.mem_op`, which the
  kernel populates from IBS OP DATA3 — a hardware classification, not
  a software heuristic.
