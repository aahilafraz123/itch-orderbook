#!/usr/bin/env bash
# Benchmark + profile on real Linux hardware (bare metal or a VM with PMU access).
#
# Usage: scripts/bench_linux.sh <itch_file> [cpu]
#   itch_file  decompressed ITCH 5.0 file (scripts/fetch_sample.sh download && ... decompress)
#   cpu        core to pin to with taskset (default 2)
#
# Writes everything to results/linux-<hostname>-<date>/:
#   machine.txt       CPU model, kernel, compiler, governor
#   bench.txt         all book variants, 5 runs each (median reported) + latency
#   perf_stat.txt     hardware counters for the fast book: cycles, instructions, IPC,
#                     cache misses, branch misses
#   perf_report.txt   hottest functions (perf record)
#   pipeline.txt      single-thread vs 2-thread SPSC pipeline (pinned to cpu, cpu+1)
#   differential.txt  fast book vs reference book over the whole file
set -euo pipefail

FILE="${1:?usage: $0 <itch_file> [cpu]}"
CPU="${2:-2}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO_ROOT/results/linux-$(hostname -s)-$(date +%Y%m%d-%H%M)"
mkdir -p "$OUT"

echo "Building (Release, -march=native) ..."
cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build-linux" -DCMAKE_BUILD_TYPE=Release -DOB_NATIVE=ON -DOB_TESTS=OFF >/dev/null
cmake --build "$REPO_ROOT/build-linux" -j >/dev/null
B="$REPO_ROOT/build-linux"

{
    echo "cpu:      $(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
    echo "cores:    $(nproc)"
    echo "kernel:   $(uname -r)"
    echo "compiler: $(c++ --version | head -1)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu"$CPU"/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
    echo "pinned:   cpu $CPU"
    echo "date:     $(date -u +%Y-%m-%dT%H:%MZ)"
} | tee "$OUT/machine.txt"

echo "Benchmark (5 runs per book) ..."
taskset -c "$CPU" "$B/bench" "$FILE" --runs 5 | tee "$OUT/bench.txt"

if command -v perf >/dev/null; then
    echo "perf stat ..."
    taskset -c "$CPU" perf stat -e cycles,instructions,cache-references,cache-misses,branches,branch-misses,L1-dcache-load-misses \
        "$B/bench" "$FILE" --book fast --runs 1 --no-latency 2>&1 | tee "$OUT/perf_stat.txt"
    echo "perf record ..."
    taskset -c "$CPU" perf record -g -o "$OUT/perf.data" "$B/bench" "$FILE" --book fast --runs 1 --no-latency >/dev/null
    perf report -i "$OUT/perf.data" --no-children --stdio --percent-limit 1 2>/dev/null | head -60 > "$OUT/perf_report.txt"
else
    echo "perf not installed (apt install linux-tools-\$(uname -r)); skipping counters" | tee "$OUT/perf_stat.txt"
fi

echo "Pipeline ..."
"$B/pipeline" "$FILE" --cpus "$CPU,$((CPU + 1))" | tee "$OUT/pipeline.txt"

echo "Differential (whole file) ..."
"$B/differential" "$FILE" | tee "$OUT/differential.txt"

echo "Done: $OUT"
