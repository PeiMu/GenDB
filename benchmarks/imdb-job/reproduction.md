# Reproducing GenDB on the JOB Benchmark

## Prerequisites

- Node.js 18+
- g++ with C++17 and OpenMP support
- Python 3 with venv
- IMDB CSV data at `/home/pei/Project/benchmarks/imdb_job-postgres/csv/` (~3.7 GB)
- Claude Code CLI with active subscription

## Directory Layout

```
GenDB/benchmarks/imdb-job/
  schema.sql              # 21-table IMDB schema
  queries.sql             # 113 JOB queries (Q1a–Q33c), concatenated
  data/sf1/               # symlink → .../imdb_job-postgres/csv/
  query_results/          # ground truth CSVs (one per query)
  generate_ground_truth.py
  .venv/                  # Python venv with DuckDB 1.4.4
```

## Step 0: Verify Setup

```bash
cd /home/pei/Project/GenDB

# Confirm data symlink resolves
ls benchmarks/imdb-job/data/sf1/title.csv

# Confirm queries parse correctly (should print 113 queries, Q1a–Q33c)
node -e "
import { parseQueryFile } from './src/gendb/shared.mjs';
import { readFileSync } from 'fs';
const q = parseQueryFile(readFileSync('benchmarks/imdb-job/queries.sql', 'utf-8'));
console.log(q.length, 'queries:', q[0].id, '...', q[q.length-1].id);
"

# Confirm provider is registered
node -e "
import('./src/gendb/providers/index.mjs').then(m =>
  console.log('Providers:', m.getAvailableProviders())
);
"
# Expected: [ 'claude', 'codex', 'claude-code' ]

# Confirm Claude Code CLI is authenticated
claude --version
```

## Step 1: Generate Ground Truth

Ground truth lets GenDB verify correctness of generated C++ code against DuckDB results.

```bash
# Create venv and install DuckDB (one-time)
python3 -m venv benchmarks/imdb-job/.venv
benchmarks/imdb-job/.venv/bin/pip install duckdb==1.4.4

# Generate ground truth for all 113 queries (~2 min)
benchmarks/imdb-job/.venv/bin/python3 benchmarks/imdb-job/generate_ground_truth.py
```

Expected output: 113 CSV files in `benchmarks/imdb-job/query_results/`, all queries succeed.

## Step 2: Run GenDB Pipeline

Close other Claude Code sessions (IDE extensions, terminal sessions) first — the spawned agents share your subscription's rate limit.

```bash
# Conservative first run (1 query at a time, fewer iterations)
node src/gendb/orchestrator.mjs \
  --benchmark imdb-job \
  --sf 1 \
  --agent-provider claude-code \
  --max-concurrent 1 \
  --max-iterations 2

# Once validated, scale up slightly
node src/gendb/orchestrator.mjs \
  --benchmark imdb-job \
  --sf 1 \
  --agent-provider claude-code \
  --max-concurrent 2 \
  --max-iterations 3

# Full run
node src/gendb/orchestrator.mjs \
  --benchmark imdb-job \
  --sf 1 \
  --agent-provider claude-code
```

### Common Options

```
--max-iterations N        Max optimization iterations per query (default: 5)
--max-concurrent N        Max parallel query optimization (default: 22, recommend 1–3 for claude-code)
--stall-threshold N       Stop after N non-improving iterations (default: 5)
--optimization-target X   "hot" (avg of repeated runs) or "cold" (single run)
--model M                 Override model for all agents (opus, sonnet, haiku)
--reoptimize <id|all>     Force re-optimization of specific or all queries
```

## Pipeline Stages

GenDB runs a 5-agent pipeline for each query:

1. **Workload Analyzer** — profiles hardware (CPU, cache, SIMD), samples CSV data, extracts join patterns and selectivity across all 113 queries
2. **Storage/Index Designer** — designs columnar binary storage with encoding (dict, fixed-int, compressed), indexes (hash, zone maps, bloom filters), generates and runs `ingest.cpp` + `build_indexes.cpp`
3. **Query Planner** — produces `plan.json` per query: join order, physical strategies, parallelism config
4. **Code Generator** — implements plan as optimized standalone C++ with `GENDB_PHASE` timing macros
5. **Query Optimizer** — iteratively refines: compiles, runs, profiles, diagnoses bottleneck, revises plan, regenerates code (up to `--max-iterations` rounds)

Phase 1 (steps 1–2) runs once for the entire workload. Phase 2 (steps 3–5) runs per query, with up to `--max-concurrent` queries in parallel.

## Output Structure

Results are written to `output/imdb-job-sf1/`:

```
output/imdb-job-sf1/
  workload_analysis.json          # Hardware + data profile
  storage/
    storage_design.json           # Column encodings, indexes, sort orders
    ingest/ingest.cpp             # Data ingestion code
    ingest/build_indexes.cpp      # Index building code
    <table>.bin, *.idx, ...       # Binary data + index files
  Q1a/
    iter0/
      plan.json                   # Execution plan
      Q1a.cpp                     # Generated C++ code
      Q1a                         # Compiled binary
      execution_results.json      # Timing breakdown
      results.csv                 # Query output
    iter1/ ...                    # Optimization iterations
    best/                         # Symlink to best iteration
  Q1b/ ...
  runs/
    <run-id>/run.json             # Run metadata, per-query timings, costs
    latest -> <run-id>            # Symlink to most recent run
```

## Verifying Results

```bash
# Check a single query's correctness against ground truth
python3 src/gendb/tools/compare_results.py \
  output/imdb-job-sf1/Q1a/best/results.csv \
  benchmarks/imdb-job/query_results/Q1a.csv

# Check timing for a query
cat output/imdb-job-sf1/Q1a/best/execution_results.json
```

## Troubleshooting

### "Not logged in"
Run `claude auth` to authenticate your Claude Code CLI.

### Rate limiting
The `claude-code` provider spawns `claude -p` subprocesses that share your subscription's rate limit. Keep `--max-concurrent` at 1–3. Close other Claude Code sessions before running.

### Storage Designer timeout
Data ingestion for 3.7 GB of IMDB data takes time. The storage designer has a 45-minute timeout. If it fails, re-run — Phase 1 artifacts are persisted and reused on subsequent runs.

### CSV quoting in IMDB data
IMDB text fields contain embedded commas and quotes (movie titles, character names). The storage designer agent detects this via sampling and generates C++ ingestion code with proper CSV parsing. If ingestion produces wrong row counts, check the generated `ingest.cpp` for correct quote/escape handling.

### Re-running after a failure
GenDB persists all Phase 1 artifacts (storage, indexes) and per-query best results. On re-run, it skips already-completed work. Use `--reoptimize all` to force re-optimization of all queries.

## Benchmarking

After the pipeline completes, measure performance with `benchmark_queries.sh`. This uses [hyperfine](https://github.com/sharkdp/hyperfine) with 5 warmup + 10 measured runs and reports mean execution time, matching the protocol used for DuckDB and Bespoke baselines.

```bash
# Install hyperfine (one-time)
sudo apt install hyperfine

# Hot mode: data served from OS page cache (warm after 5 warmup runs)
bash benchmark_queries.sh --mode hot

# Cold mode: OS page cache flushed before each run (requires sudo)
bash benchmark_queries.sh --mode cold
```

Results are written to `output/imdb-job-sf1/benchmark_results/`:
- `gendb_hot.csv` / `gendb_cold.csv` — per-query mean, stddev, min, max (ms)
- `individual_hot/` / `individual_cold/` — per-query hyperfine JSON with full run data
