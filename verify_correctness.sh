#!/usr/bin/env bash
# Verify correctness of all GenDB-generated binaries against ground truth.
# Recompiles all C++ sources from best/ before running (ensures binary compatibility).
# Usage: bash verify_correctness.sh

set -euo pipefail

GENDB_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKLOAD_DIR="$GENDB_DIR/output/imdb-job-sf1"
STORAGE="$WORKLOAD_DIR/storage"
GROUND_TRUTH="$GENDB_DIR/benchmarks/imdb-job/query_results"
COMPARE="$GENDB_DIR/src/gendb/tools/compare_results.py"
INCL="$GENDB_DIR/src/gendb/utils"
RESULTS_DIR=$(mktemp -d)
trap 'rm -rf "$RESULTS_DIR"' EXIT

CXX_FLAGS="-O3 -march=native -std=c++17 -fopenmp -lpthread"

echo "=== GenDB Correctness Verification ==="
echo "Workload:     $WORKLOAD_DIR"
echo "Storage:      $STORAGE"
echo "Ground truth: $GROUND_TRUTH"
echo ""

# Step 1: Recompile all queries from source
echo "=== Recompiling all queries ==="
compile_ok=0
compile_fail=0
for best_dir in "$WORKLOAD_DIR"/queries/Q*/best/; do
    qid=$(basename "$(dirname "$best_dir")")
    ql=$(echo "$qid" | tr '[:upper:]' '[:lower:]')
    cpp="$best_dir/${ql}.cpp"
    bin="$best_dir/${ql}"
    if [ ! -f "$cpp" ]; then
        continue
    fi
    if g++ $CXX_FLAGS -I"$INCL" "$cpp" -o "$bin" 2>/dev/null; then
        compile_ok=$((compile_ok + 1))
    else
        printf "%-8s COMPILE ERROR\n" "$qid"
        compile_fail=$((compile_fail + 1))
    fi
done
echo "Compiled: $compile_ok, Failed: $compile_fail"
echo ""

# Step 2: Verify correctness
echo "=== Verifying correctness ==="
pass=0
fail=0
no_binary=0
fail_list=""
no_binary_list=""

for gt_csv in "$GROUND_TRUTH"/Q*.csv; do
    qid=$(basename "${gt_csv%.csv}")
    ql=$(echo "$qid" | tr '[:upper:]' '[:lower:]')
    binary="$WORKLOAD_DIR/queries/$qid/best/$ql"

    if [ ! -x "$binary" ]; then
        printf "%-8s NO BINARY\n" "$qid"
        no_binary=$((no_binary + 1))
        no_binary_list="$no_binary_list $qid"
        continue
    fi

    qresults="$RESULTS_DIR/$qid"
    mkdir -p "$qresults"
    if ! "$binary" "$STORAGE" "$qresults" >/dev/null 2>&1; then
        printf "%-8s FAIL (runtime error)\n" "$qid"
        fail=$((fail + 1))
        fail_list="$fail_list $qid"
        continue
    fi

    actual_csv="$qresults/${qid}.csv"
    if [ ! -f "$actual_csv" ]; then
        printf "%-8s FAIL (no output CSV)\n" "$qid"
        fail=$((fail + 1))
        fail_list="$fail_list $qid"
        continue
    fi

    exp_tmp="$RESULTS_DIR/_exp_$qid"
    act_tmp="$RESULTS_DIR/_act_$qid"
    mkdir -p "$exp_tmp" "$act_tmp"
    cp "$gt_csv" "$exp_tmp/"
    cp "$actual_csv" "$act_tmp/"

    result=$(python3 "$COMPARE" "$exp_tmp" "$act_tmp" 2>&1)
    match=$(echo "$result" | python3 -c "import json,sys; print(json.load(sys.stdin).get('match', False))" 2>/dev/null || echo "False")

    rm -rf "$exp_tmp" "$act_tmp"

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
echo "PASS:      $pass / 113"
echo "FAIL:      $fail"
echo "NO BINARY: $no_binary"
if [ -n "$fail_list" ]; then
    echo "Failed:$fail_list"
fi
if [ -n "$no_binary_list" ]; then
    echo "No binary:$no_binary_list"
fi
