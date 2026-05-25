#!/usr/bin/env bash
# reproduction_job.sh — Reproduce GenDB on the JOB (Join Order Benchmark)
#
# Usage:
#   ./reproduction_job.sh                  # full run with defaults
#   ./reproduction_job.sh --max-concurrent 1 --max-iterations 2   # conservative
#
# Prerequisites:
#   - Node.js 18+, g++ (C++17 + OpenMP), Python 3
#   - Claude Code CLI authenticated (run `claude auth` first)
#   - IMDB CSV data at ../benchmarks/imdb_job-postgres/csv/
#
# All extra arguments are forwarded to the orchestrator.

set -euo pipefail
cd "$(dirname "$0")"

BENCHMARK_DIR="benchmarks/imdb-job"
DATA_SOURCE="/home/pei/Project/benchmarks/imdb_job-postgres/csv"
QUERIES_SOURCE="/home/pei/Project/benchmarks/imdb_job-postgres/queries"
VENV_DIR="$BENCHMARK_DIR/.venv"

# ── Preflight checks ────────────────────────────────────────────────

echo "=== Preflight checks ==="

command -v node >/dev/null   || { echo "ERROR: node not found";   exit 1; }
command -v g++ >/dev/null    || { echo "ERROR: g++ not found";    exit 1; }
command -v python3 >/dev/null || { echo "ERROR: python3 not found"; exit 1; }
command -v claude >/dev/null || { echo "ERROR: claude CLI not found"; exit 1; }

if [ ! -d "$DATA_SOURCE" ]; then
  echo "ERROR: IMDB CSV data not found at $DATA_SOURCE"
  exit 1
fi

echo "All prerequisites found."

# ── Step 1: Set up benchmark directory ───────────────────────────────

echo ""
echo "=== Step 1: Setting up benchmark directory ==="

mkdir -p "$BENCHMARK_DIR/data" "$BENCHMARK_DIR/query_results"

# Schema
if [ ! -f "$BENCHMARK_DIR/schema.sql" ]; then
  cp /home/pei/Project/benchmarks/imdb_job-postgres/schema.sql "$BENCHMARK_DIR/schema.sql"
  echo "Copied schema.sql"
else
  echo "schema.sql already exists, skipping"
fi

# Data symlink
if [ ! -e "$BENCHMARK_DIR/data/sf1" ]; then
  ln -s "$DATA_SOURCE" "$BENCHMARK_DIR/data/sf1"
  echo "Created data symlink"
else
  echo "Data symlink already exists, skipping"
fi

# Concatenate queries
if [ ! -f "$BENCHMARK_DIR/queries.sql" ]; then
  echo "Concatenating 113 JOB queries..."
  for f in $(ls "$QUERIES_SOURCE"/*.sql | sort -V); do
    id="$(basename "${f%.sql}")"
    echo "-- Q${id}"
    cat "$f"
    echo ""
  done > "$BENCHMARK_DIR/queries.sql"
  count=$(grep -c "^-- Q" "$BENCHMARK_DIR/queries.sql")
  echo "Generated queries.sql ($count queries)"
else
  echo "queries.sql already exists, skipping"
fi

# ── Step 2: Generate ground truth ────────────────────────────────────

echo ""
echo "=== Step 2: Generating ground truth ==="

existing=$(ls "$BENCHMARK_DIR/query_results/"*.csv 2>/dev/null | wc -l || true)
if [ "$existing" -ge 113 ]; then
  echo "Ground truth already complete ($existing files), skipping"
else
  if [ ! -d "$VENV_DIR" ]; then
    echo "Creating Python venv..."
    python3 -m venv "$VENV_DIR"
  fi

  if ! "$VENV_DIR/bin/python3" -c "import duckdb" 2>/dev/null; then
    echo "Installing DuckDB 1.4.4..."
    "$VENV_DIR/bin/pip" install -q duckdb==1.4.4
  fi

  if ! "$VENV_DIR/bin/python3" -c "import sqlglot" 2>/dev/null; then
    echo "Installing sqlglot (for SQL template extraction)..."
    "$VENV_DIR/bin/pip" install -q sqlglot
  fi

  echo "Running ground truth generation (this loads ~3.7 GB of data)..."
  "$VENV_DIR/bin/python3" "$BENCHMARK_DIR/generate_ground_truth.py"
fi

# ── Step 3: Verify query parsing ─────────────────────────────────────

echo ""
echo "=== Step 3: Verifying query parsing ==="

node --input-type=module -e "
import { parseQueryFile } from './src/gendb/shared.mjs';
import { readFileSync } from 'fs';
const q = parseQueryFile(readFileSync('$BENCHMARK_DIR/queries.sql', 'utf-8'));
if (q.length !== 113) {
  console.error('ERROR: expected 113 queries, got ' + q.length);
  process.exit(1);
}
console.log('Parsed ' + q.length + ' queries: ' + q[0].id + ' ... ' + q[q.length-1].id);
"

# ── Step 4: Run GenDB pipeline ───────────────────────────────────────

echo ""
echo "=== Step 4: Running GenDB pipeline ==="
echo "Provider: claude-code"
echo "Close other Claude Code sessions to avoid rate limit contention."
echo ""

# Activate venv so the orchestrator's python3 calls find sqlglot, duckdb, etc.
export PATH="$PWD/$VENV_DIR/bin:$PATH"

node src/gendb/orchestrator.mjs \
  --benchmark imdb-job \
  --sf 1 \
  --agent-provider claude-code \
  "$@"

echo ""
echo "=== Done ==="
echo "Pipeline output: output/imdb-job-sf1/"
echo ""
echo "Next: verify correctness, then measure performance:"
echo "  bash verify_correctness.sh"
echo "  python3 $BENCHMARK_DIR/measure_gendb_warm.py"
echo "  python3 $BENCHMARK_DIR/measure_gendb_cold.py"
