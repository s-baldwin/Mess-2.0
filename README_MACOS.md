# MESS 2.0 on Apple Silicon macOS

This fork (`macos-port` branch) builds and runs MESS 2.0 natively on Apple Silicon Macs.

Tested on an M3 Pro, macOS 26.6.2, Apple clang 21.0.0, upstream MESS commit `1320352`. Other Apple Silicon chips should work but haven't been tested, so run the test script below on your machine first.

> Developed with help from an AI assistant (Claude) and shared with TA permission. Mention it in your report if you use it.

## 1. Get the code

**Option A: clone the fork**

```bash
git clone https://github.com/s-baldwin/Mess-2.0.git Mess-2.0
cd Mess-2.0
git checkout macos-port
```

**Option B: apply the patch to upstream**

```bash
git clone --recursive https://github.com/bsc-mem/Mess-2.0.git
cd Mess-2.0
git checkout 1320352
git apply macos-port.patch     # path to the patch file you downloaded
```

You need the Xcode Command Line Tools (`xcode-select --install`). No `sudo` is needed.

## 2. Build

```bash
make
MESS_TRAFFICGEN_ARRAY_MB=<size> make install
```

`MESS_TRAFFICGEN_ARRAY_MB` sets the size of each traffic-generator array in MB. macOS doesn't report the last-level cache, so MESS can't size the arrays itself. Pick a size well above your chip's total cache, including the system-level cache (SLC); otherwise the traffic hits cache instead of DRAM. Check it took effect with:

```bash
grep "array:" install.log      # if you piped make install to install.log
```

If you change the size later, re-run `make install`.

## 3. Check that it works

```bash
bash test_macos_port.sh quick     # ~2 min
bash test_macos_port.sh           # ~15–25 min, optional full check
```

Every line should say `PASS`. Keep the Mac plugged in and idle while it runs.

## 4. Take a reading

**Quick test** (one read/write mix, two load levels):

```bash
./build/bin/mess --ratio=100 --pause=0,100 --repetitions=1
```

**A full bandwidth–latency curve** for 100% reads, using MESS's adaptive pause selection:

```bash
./build/bin/mess --ratio=100 --tier=lite --repetitions=3 --profile
```

**Several read/write mixes:**

```bash
./build/bin/mess --ratio=100,75,50 --tier=lite --repetitions=3 --profile
```

With `--profile`, the bandwidth and latency files are saved under `measuring/` (change it with `--folder=DIR`).

Runtime tips:
- Each point takes a few seconds, so full default runs (all 51 ratios × 50 points × 3 repetitions) take many hours. Use `--ratio`, `--tier=lite|standard|detailed`, and `--repetitions` to control the run size.
- `--verbose=3` shows the per-sample details if you want to watch progress.
- If a run is interrupted, clean up with `pkill -f traffic_gen_multiseq; rm -rf /tmp/mess_sw_bw`.

## 5. What's different from MESS on Linux

Know these before you report numbers:

- **Bandwidth** is the number of bytes the traffic generators issued, counted in software. It isn't DRAM CAS counters, because macOS doesn't expose them.
- **Latency** is pointer-chase wall-clock time per load. There's no TLB-miss correction; the "TLB hit latency: 1 ns" MESS prints is a placeholder.
- **No core pinning, memory binding, or huge pages.** The "Huge page allocation success" message is misleading on macOS; pages are 16 KB.
- **Options with no effect on macOS:** `--bind`, `--add-counters`, and `--measurer`. `--cores` only sets how many generators run, not which cores they use. `--inst-lat` is unsupported.

## Updating an existing copy

If you already have the fork and it gets updated:

```bash
cd Mess-2.0
git pull
make
MESS_TRAFFICGEN_ARRAY_MB=<size> make install
bash test_macos_port.sh quick
```

Always re-run `make install` after pulling. It regenerates the benchmark kernels, and a stale kernel can silently give wrong results.

## Troubleshooting

| Problem | Fix |
|---|---|
| Latency is 0, or `dur: 0.000 s` at `--verbose=3` | Kernels are stale. Re-run `make` and `make install`. |
| Very high bandwidth from just a few cores | Arrays too small (hitting cache). Increase `MESS_TRAFFICGEN_ARRAY_MB` and re-run `make install`. |
| Strange bandwidth after an interrupted run | `pkill -f traffic_gen_multiseq; rm -rf /tmp/mess_sw_bw` |
| A point warns "BW did not stabilize" | Try `MESS_MAC_BW_WINDOW_MS=1000 ./build/bin/mess ...` (default 500 ms). |
