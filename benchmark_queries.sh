#!/usr/bin/env bash
# Benchmark all passing GenDB-generated binaries using hyperfine.
# Usage: bash benchmark_queries.sh [run-dir] [--warmup N] [--runs N]
#   run-dir:    path to a run directory (default: latest run)
#   --warmup N: warmup runs (default: 5)
#   --runs N:   measured runs (default: 10)

set -euo pipefail

GENDB_DIR="$(cd "$(dirname "$0")" && pwd)"
STORAGE="$GENDB_DIR/output/imdb-job-sf1/storage"
GROUND_TRUTH="$GENDB_DIR/benchmarks/imdb-job/query_results"
COMPARE="$GENDB_DIR/src/gendb/tools/compare_results.py"
RESULTS_DIR=$(mktemp -d)
trap 'rm -rf "$RESULTS_DIR"' EXIT

WARMUP=5
RUNS=10
RUN_DIR=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --warmup) WARMUP="$2"; shift 2 ;;
        --runs)   RUNS="$2"; shift 2 ;;
        *)
            if [ -z "$RUN_DIR" ]; then
                RUN_DIR="$1"
            fi
            shift ;;
    esac
done

RUN_DIR="${RUN_DIR:-$GENDB_DIR/output/imdb-job-sf1/runs/latest}"
RUN_DIR="$(readlink -f "$RUN_DIR")"

echo "=== GenDB Performance Benchmark ==="
echo "Run:     $RUN_DIR"
echo "Storage: $STORAGE"
echo "Warmup:  $WARMUP"
echo "Runs:    $RUNS"
echo ""

if ! command -v hyperfine &>/dev/null; then
    echo "ERROR: hyperfine not found. Install with: sudo apt install hyperfine"
    exit 1
fi

if [ ! -f "$RUN_DIR/run.json" ]; then
    echo "ERROR: run.json not found in $RUN_DIR"
    exit 1
fi

# Collect all query IDs from ground truth
all_qids=()
for f in "$GROUND_TRUTH"/Q*.csv; do
    qid=$(basename "${f%.csv}")
    all_qids+=("$qid")
done
IFS=$'\n' all_qids=($(sort <<<"${all_qids[*]}")); unset IFS

# Phase 1: identify passing queries
echo "=== Phase 1: Identifying correct binaries ==="
passing=()

for qid in "${all_qids[@]}"; do
    qid_lower=$(echo "$qid" | tr '[:upper:]' '[:lower:]')
    qdir="$RUN_DIR/queries/$qid"
    [ -d "$qdir" ] || continue

    binary=""
    for iter_dir in $(find "$qdir" -mindepth 1 -maxdepth 1 -name "iter_*" -type d 2>/dev/null | sort -t_ -k2 -n -r); do
        candidate="$iter_dir/$qid_lower"
        if [ -x "$candidate" ]; then
            binary="$candidate"
            break
        fi
    done

    [ -z "$binary" ] && continue

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
echo "=== Phase 2: Benchmarking with hyperfine ==="
BENCH_OUT="$GENDB_DIR/output/imdb-job-sf1/benchmark_results"
mkdir -p "$BENCH_OUT"

INDIVIDUAL_DIR="$BENCH_OUT/individual"
mkdir -p "$INDIVIDUAL_DIR"

for entry in "${passing[@]}"; do
    qid="${entry%%:*}"
    binary="${entry#*:}"
    qresults="$RESULTS_DIR/bench_$qid"
    mkdir -p "$qresults"

    echo ""
    echo "--- $qid ---"
    hyperfine \
        --warmup "$WARMUP" \
        --runs "$RUNS" \
        --export-json "$INDIVIDUAL_DIR/${qid}.json" \
        "$binary $STORAGE $qresults"
done

# Phase 3: combined summary
echo ""
echo "=== Phase 3: Summary ==="
echo ""
printf "%-8s %12s %12s %12s\n" "Query" "Mean (ms)" "Min (ms)" "Max (ms)"
printf "%-8s %12s %12s %12s\n" "--------" "------------" "------------" "------------"

total_mean=0
count=0

for entry in "${passing[@]}"; do
    qid="${entry%%:*}"
    jf="$INDIVIDUAL_DIR/${qid}.json"
    [ ! -f "$jf" ] && continue

    read -r mean_s min_s max_s < <(python3 -c "
import json
d = json.load(open('$jf'))
r = d['results'][0]
print(f\"{r['mean']*1000:.2f} {r['min']*1000:.2f} {r['max']*1000:.2f}\")
" 2>/dev/null)

    printf "%-8s %12s %12s %12s\n" "$qid" "$mean_s" "$min_s" "$max_s"
    total_mean=$(python3 -c "print($total_mean + $mean_s)")
    count=$((count + 1))
done

echo ""
echo "Total (sum of means): ${total_mean} ms across $count queries"
echo "Individual results:   $INDIVIDUAL_DIR/"
