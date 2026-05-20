#!/usr/bin/env bash
# Verify correctness of all GenDB-generated binaries against ground truth.
# Usage: bash verify_correctness.sh [run-dir]
#   run-dir: path to a run directory (default: latest run)

set -euo pipefail

GENDB_DIR="$(cd "$(dirname "$0")" && pwd)"
STORAGE="$GENDB_DIR/output/imdb-job-sf1/storage"
GROUND_TRUTH="$GENDB_DIR/benchmarks/imdb-job/query_results"
COMPARE="$GENDB_DIR/src/gendb/tools/compare_results.py"
RESULTS_DIR=$(mktemp -d)

RUN_DIR="${1:-$GENDB_DIR/output/imdb-job-sf1/runs/latest}"
RUN_DIR="$(readlink -f "$RUN_DIR")"

echo "=== GenDB Correctness Verification ==="
echo "Run:          $RUN_DIR"
echo "Storage:      $STORAGE"
echo "Ground truth: $GROUND_TRUTH"
echo "Temp results: $RESULTS_DIR"
echo ""

if [ ! -f "$RUN_DIR/run.json" ]; then
    echo "ERROR: run.json not found in $RUN_DIR"
    exit 1
fi

pass=0
fail=0
skip=0
fail_list=""

query_dirs=$(find "$RUN_DIR/queries" -mindepth 1 -maxdepth 1 -type d | sort)

for qdir in $query_dirs; do
    qid=$(basename "$qdir")
    qid_lower=$(echo "$qid" | tr '[:upper:]' '[:lower:]')

    # Find the best binary: highest iter with a compiled binary
    binary=""
    for iter_dir in $(find "$qdir" -mindepth 1 -maxdepth 1 -name "iter_*" -type d | sort -t_ -k2 -n -r); do
        candidate="$iter_dir/$qid_lower"
        if [ -x "$candidate" ]; then
            binary="$candidate"
            break
        fi
    done

    if [ -z "$binary" ]; then
        printf "%-8s SKIP (no binary)\n" "$qid"
        skip=$((skip + 1))
        continue
    fi

    # Run the binary
    qresults="$RESULTS_DIR/$qid"
    mkdir -p "$qresults"
    if ! "$binary" "$STORAGE" "$qresults" >/dev/null 2>&1; then
        printf "%-8s FAIL (runtime error)\n" "$qid"
        fail=$((fail + 1))
        fail_list="$fail_list $qid"
        continue
    fi

    # Check that the output CSV exists
    expected_csv="$GROUND_TRUTH/${qid}.csv"
    actual_csv="$qresults/${qid}.csv"
    if [ ! -f "$actual_csv" ]; then
        printf "%-8s FAIL (no output CSV)\n" "$qid"
        fail=$((fail + 1))
        fail_list="$fail_list $qid"
        continue
    fi

    if [ ! -f "$expected_csv" ]; then
        printf "%-8s SKIP (no ground truth)\n" "$qid"
        skip=$((skip + 1))
        continue
    fi

    # Compare using the tool (it expects directories, so create single-file dirs)
    exp_tmp="$RESULTS_DIR/_exp_$qid"
    act_tmp="$RESULTS_DIR/_act_$qid"
    mkdir -p "$exp_tmp" "$act_tmp"
    cp "$expected_csv" "$exp_tmp/"
    cp "$actual_csv" "$act_tmp/"

    result=$(python3 "$COMPARE" "$exp_tmp" "$act_tmp" 2>&1)
    match=$(echo "$result" | python3 -c "import json,sys; print(json.load(sys.stdin).get('match', False))" 2>/dev/null || echo "False")

    if [ "$match" = "True" ]; then
        printf "%-8s PASS\n" "$qid"
        pass=$((pass + 1))
    else
        printf "%-8s FAIL (wrong results)\n" "$qid"
        fail=$((fail + 1))
        fail_list="$fail_list $qid"
    fi
done

echo ""
echo "=== Summary ==="
echo "PASS: $pass"
echo "FAIL: $fail"
echo "SKIP: $skip"
echo "Total: $((pass + fail + skip))"
if [ -n "$fail_list" ]; then
    echo "Failed queries:$fail_list"
fi

rm -rf "$RESULTS_DIR"
