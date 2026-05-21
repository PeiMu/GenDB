#!/usr/bin/env bash
# Benchmark all passing GenDB-generated binaries using hyperfine.
# Usage: bash benchmark_queries.sh [--mode hot|cold] [--warmup N] [--runs N]
#   --mode hot:   no cache clearing (default)
#   --mode cold:  flush OS page cache before each run (requires sudo)
#   --warmup N:   warmup runs (default: 5)
#   --runs N:     measured runs (default: 10)

set -euo pipefail

GENDB_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKLOAD_DIR="$GENDB_DIR/output/imdb-job-sf1"
STORAGE="$WORKLOAD_DIR/storage"
GROUND_TRUTH="$GENDB_DIR/benchmarks/imdb-job/query_results"
COMPARE="$GENDB_DIR/src/gendb/tools/compare_results.py"
RESULTS_DIR=$(mktemp -d)
trap 'rm -rf "$RESULTS_DIR"' EXIT

WARMUP=5
RUNS=10
MODE="hot"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --warmup) WARMUP="$2"; shift 2 ;;
        --runs)   RUNS="$2"; shift 2 ;;
        --mode)   MODE="$2"; shift 2 ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

if [[ "$MODE" != "hot" && "$MODE" != "cold" ]]; then
    echo "ERROR: --mode must be 'hot' or 'cold'"
    exit 1
fi

echo "=== GenDB Performance Benchmark ==="
echo "Workload: $WORKLOAD_DIR"
echo "Storage:  $STORAGE"
echo "Mode:     $MODE"
echo "Warmup:   $WARMUP"
echo "Runs:     $RUNS"
echo ""

if ! command -v hyperfine &>/dev/null; then
    echo "ERROR: hyperfine not found. Install with: sudo apt install hyperfine"
    exit 1
fi

if [[ "$MODE" == "cold" ]]; then
    if ! sudo -n sh -c "sync && echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null; then
        echo "ERROR: cold mode requires passwordless sudo for drop_caches."
        echo "Add to sudoers: $(whoami) ALL=(ALL) NOPASSWD: /bin/sh -c sync && echo 3 > /proc/sys/vm/drop_caches"
        exit 1
    fi
    echo "OS cache clearing verified."
    echo ""
fi

# Collect all query IDs from ground truth
all_qids=()
for f in "$GROUND_TRUTH"/Q*.csv; do
    qid=$(basename "${f%.csv}")
    all_qids+=("$qid")
done
IFS=$'\n' all_qids=($(sort -V <<<"${all_qids[*]}")); unset IFS

# Phase 1: identify passing queries (use best/ promoted binaries)
echo "=== Phase 1: Identifying correct binaries ==="
passing=()

for qid in "${all_qids[@]}"; do
    qid_lower=$(echo "$qid" | tr '[:upper:]' '[:lower:]')
    best_dir="$WORKLOAD_DIR/queries/$qid/best"
    binary="$best_dir/$qid_lower"

    if [ ! -x "$binary" ]; then
        continue
    fi

    qresults="$RESULTS_DIR/$qid"
    mkdir -p "$qresults"
    if ! "$binary" "$STORAGE" "$qresults" >/dev/null 2>&1; then
        continue
    fi

    expected_csv="$GROUND_TRUTH/${qid}.csv"
    actual_csv="$qresults/${qid}.csv"
    [ ! -f "$actual_csv" ] && continue
    [ ! -f "$expected_csv" ] && continue

    exp_tmp="$RESULTS_DIR/_exp_$qid"
    act_tmp="$RESULTS_DIR/_act_$qid"
    mkdir -p "$exp_tmp" "$act_tmp"
    cp "$expected_csv" "$exp_tmp/"
    cp "$actual_csv" "$act_tmp/"

    match=$(python3 "$COMPARE" "$exp_tmp" "$act_tmp" 2>/dev/null \
        | python3 -c "import json,sys; print(json.load(sys.stdin).get('match', False))" 2>/dev/null || echo "False")

    rm -rf "$exp_tmp" "$act_tmp"

    if [ "$match" = "True" ]; then
        passing+=("$qid:$binary")
        printf "  %-8s PASS -> will benchmark\n" "$qid"
    fi
done

echo ""
echo "${#passing[@]} / ${#all_qids[@]} queries passed correctness check."
echo ""

if [ ${#passing[@]} -eq 0 ]; then
    echo "No passing queries to benchmark."
    exit 0
fi

# Phase 2: benchmark each passing query individually
echo "=== Phase 2: Benchmarking with hyperfine (mode=$MODE) ==="
BENCH_OUT="$WORKLOAD_DIR/benchmark_results"
mkdir -p "$BENCH_OUT"

INDIVIDUAL_DIR="$BENCH_OUT/individual_${MODE}"
mkdir -p "$INDIVIDUAL_DIR"

PREPARE_CMD=""
if [[ "$MODE" == "cold" ]]; then
    PREPARE_CMD='sudo sh -c "sync && echo 3 > /proc/sys/vm/drop_caches"'
fi

for entry in "${passing[@]}"; do
    qid="${entry%%:*}"
    binary="${entry#*:}"
    qresults="$RESULTS_DIR/bench_$qid"
    mkdir -p "$qresults"

    echo ""
    echo "--- $qid ---"
    if [[ -n "$PREPARE_CMD" ]]; then
        hyperfine \
            --warmup "$WARMUP" \
            --runs "$RUNS" \
            --prepare "$PREPARE_CMD" \
            --export-json "$INDIVIDUAL_DIR/${qid}.json" \
            "$binary $STORAGE $qresults"
    else
        hyperfine \
            --warmup "$WARMUP" \
            --runs "$RUNS" \
            --export-json "$INDIVIDUAL_DIR/${qid}.json" \
            "$binary $STORAGE $qresults"
    fi
done

# Phase 3: summary table + CSV export
echo ""
echo "=== Phase 3: Summary ==="
echo ""
printf "%-8s %12s %12s %12s %12s\n" "Query" "Mean (ms)" "Stddev (ms)" "Min (ms)" "Max (ms)"
printf "%-8s %12s %12s %12s %12s\n" "--------" "------------" "------------" "------------" "------------"

CSV_OUT="$BENCH_OUT/gendb_${MODE}.csv"
echo "query,mean_ms,stddev_ms,min_ms,max_ms" > "$CSV_OUT"

total_mean=0
count=0

for entry in "${passing[@]}"; do
    qid="${entry%%:*}"
    jf="$INDIVIDUAL_DIR/${qid}.json"
    [ ! -f "$jf" ] && continue

    read -r mean_s stddev_s min_s max_s < <(python3 -c "
import json
d = json.load(open('$jf'))
r = d['results'][0]
print(f\"{r['mean']*1000:.3f} {r['stddev']*1000:.3f} {r['min']*1000:.3f} {r['max']*1000:.3f}\")
" 2>/dev/null)

    printf "%-8s %12s %12s %12s %12s\n" "$qid" "$mean_s" "$stddev_s" "$min_s" "$max_s"
    echo "$qid,$mean_s,$stddev_s,$min_s,$max_s" >> "$CSV_OUT"
    total_mean=$(python3 -c "print($total_mean + $mean_s)")
    count=$((count + 1))
done

echo ""
echo "Total (sum of means): ${total_mean} ms across $count queries"
echo "CSV output:           $CSV_OUT"
echo "Individual results:   $INDIVIDUAL_DIR/"
