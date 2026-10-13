# Benchmarking

On Linux, use the `tools/linux_reduced_cpu_variance_run.sh` wrapper
to [reduce CPU variance](https://google.github.io/benchmark/reducing_variance.html).

Timedemo:

```bash
tools/linux_reduced_cpu_variance_run.sh tools/measure_timedemo_performance.py -n 5 --binary build-rel/devilutionx
```

Individual benchmarks (built when `BUILD_TESTING` is `ON`):

```bash
tools/build_and_run_benchmark.py clx_render_benchmark
```

You can pass arguments to the benchmark binary with `--`, e.g.:

```bash
tools/build_and_run_benchmark.py clx_render_benchmark -- --benchmark_repetitions=5
```

The `tools/build_and_run_benchmark.py` script basically does something like this:

```bash
{ [ -d build-reld ] || cmake -S. -Bbuild-reld -DCMAKE_BUILD_TYPE=RelWithDebInfo; } && \
cmake --build build-reld --target clx_render_benchmark && \
tools/linux_reduced_cpu_variance_run.sh build-reld/clx_render_benchmark
```

See `tools/build_and_run_benchmark.py --help` for more information.

You can also [profile](profiling-linux.md) your benchmarks.


## Release measurement contract

The release gate is an evidence contract, not a reporting-only benchmark. Every
run must use the deterministic reference scene and emit machine-readable JSON
(or JSON Lines) containing the schema version, git revision, build type,
compiler, OS/kernel, CPU, GPU/adapter, driver, SDL3 surface/window system,
resolution, quality tier, backend, browser and browser version when applicable,
locale, power/thermal mode, and dependency/download sizes. Do not infer support
for an environment that was not run; record it as `pending` or `unsupported`.

### Reference workload and sampling

Use a fixed seed and fixed content/assets for the reference scene, fixed logical
simulation tick rate, fixed render resolution, and fixed quality tier. The
scene must be reset before each measured repetition. A run consists of at least
10 discarded warm-up frames (including one completed simulation tick) followed
by 100 measured frames/ticks, unless a matrix-specific record documents why a
larger sample is required. Capture frame time, simulation tick time,
render-submission CPU time, supported GPU-work time, startup-to-first-usable
frame time, peak resident memory, atlas bytes/pages/resident/evicted counts,
and cache/upload counters. Report arithmetic mean, p95 (nearest-rank), and
peak (maximum) for every timing and memory series. Keep startup measurements
separate from warmed steady-state measurements; never average them together.

Repeat the complete workload at least three times from a clean process. The
release result uses the worst repetition's p95 and peak, not a favorable
average. A run is invalid if its fixed seed, sample counts, environment
metadata, or required measurement fields are absent. GPU timings may be
`unsupported` only when the backend cannot expose them; the record must retain
that fact and the CPU submission timing.

### Adopted gates and result status

Until measured evidence changes them through review, the adopted reference
limits are: frame p95 <= 16.67 ms, simulation tick p95 <= 16.67 ms, warmed
render-submission p95 <= 2.00 ms, startup-to-first-usable-frame <= 5.00 s,
and peak resident memory <= 512 MiB on the low-tier profile. Atlas residency
must remain within the selected quality tier's budget, and no required cache
counter may be missing. These limits apply to the named matrix entry and are
not performance promises for untested hardware.

Each machine-readable result must include `gate_status` (`pass`, `blocked`, or
`invalid`), the adopted thresholds, measured p95/peak values, and a
`blocking_defect` when any threshold, lifecycle check, visual check, or required
measurement fails. Any missed frame, tick, startup, atlas, or memory limit
keeps release status `blocked`; do not lower a target or silently omit a
failure. A top-level aggregate may be `pass` only when every required matrix
entry has a valid passing result and lifecycle/visual evidence. Store raw JSON,
environment metadata, and dependency/download manifests under the benchmark
artifact directory named by the CI or release run.

Executable reference invocation (JSON output is mandatory):

```bash
cmake --build build-reld --target clx_render_benchmark
build-reld/clx_render_benchmark --benchmark_format=json \
  --benchmark_out=artifacts/benchmarks/reference.json \
  --benchmark_repetitions=3 --benchmark_min_warmup_time=0
```

The benchmark's release-gate mode must consume the deterministic workload and
produce the contract fields above; a human-readable Google Benchmark table
alone is insufficient. Dependency downloads are counted separately from
runtime memory and startup, with URL/version, compressed and unpacked byte
counts, and a checksum recorded where applicable.

### Required execution matrix

The evidence directory must contain one record for each attempted entry below.
An unavailable host is recorded as `pending` or `unsupported`, never as a pass
inferred from another backend:

| Class | Required entries |
| --- | --- |
| Native | D3D12; Metal; Vulkan/X11; Vulkan/Wayland |
| Browser | Each supported browser and OS version actually tested with Emscripten/SDL3 canvas |

Each record includes adapter and driver versions, OS and kernel, browser and
version when applicable, SDL3 surface, resolution, quality tier, build
revision, dependency manifest, lifecycle results (init, resize, loss/recovery,
shutdown, repeated startup), and separate classic-palette and enhanced
indexed/RGBA visual result identifiers. A missing lifecycle or visual result
makes the entry invalid and blocks release.

Use this defect format in the release evidence when a gate fails:

```text
status: blocked
entry: <backend / OS / browser>
defect: <measured failure>
metric: <name> p95=<value> peak=<value> limit=<value>
artifact: <raw result path and checksum>
```

Optimization claims require a before/after pair from the same matrix entry,
with frame/tick/submission p95, peak, startup, memory, atlas/cache, upload,
import/serialization, and wasmi measurements plus visual identifiers. Do not
publish a supported version, quality tier, memory ceiling, or performance claim
until that exact matrix entry has a passing record. The low-tier ceiling is the
adopted 512 MiB peak limit unless measured evidence and review replace it.

## Comparing benchmark runs

You can use [compare.py from Google Benchmark](https://github.com/google/benchmark/blob/main/docs/tools.md) to compare 2 benchmarks.

First, install the tool:

```bash
git clone git@github.com:google/benchmark.git ~/google-benchmark
cd ~/google-benchmark/tools
pip3 install -r requirements.txt
cd -
```

Then, build the 2 binaries that you'd like to compare. For example:

```bash
BASELINE=master
BENCHMARK=dun_render_benchmark

git checkout "$BASELINE"
tools/build_and_run_benchmark.py -B "build-reld-${BASELINE}" --no-run "$BENCHMARK"
git checkout -

tools/build_and_run_benchmark.py --no-run "$BENCHMARK"

tools/linux_reduced_cpu_variance_run.sh ~/google-benchmark/tools/compare.py -a benchmarks \
  "build-reld-${BASELINE}/${BENCHMARK}" "build-reld/${BENCHMARK}" \
  --benchmark_repetitions=10
```
