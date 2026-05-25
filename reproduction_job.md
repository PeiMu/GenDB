# Reproducing GenDB on the JOB Benchmark

## Quick Start

Full reproduction has two phases: (1) run the pipeline, (2) measure performance.

```bash
chmod +x reproduction_job.sh

# Phase 1: Pipeline (setup + ground truth + code generation)
./reproduction_job.sh --max-concurrent 1 --max-iterations 5

# Phase 2: Verify and measure (run separately after pipeline completes)
bash verify_correctness.sh
python3 benchmarks/imdb-job/measure_gendb_warm.py
python3 benchmarks/imdb-job/measure_gendb_cold.py
```

`reproduction_job.sh` is idempotent: it skips steps that are already complete (schema copied, ground truth generated, queries with existing `best/`). Extra arguments are forwarded to the orchestrator. Re-running it only processes queries that don't have a passing binary yet.

**Important:** Close other Claude Code sessions (IDE extensions, terminal tabs) before running. The `claude-code` provider spawns `claude -p` subprocesses that share your subscription's rate limit. Keep `--max-concurrent` at 1–3.

Below are the step-by-step instructions for what the script does.

---

## Step-by-Step Instructions

### Prerequisites

- Node.js 18+
- g++ with C++17 and OpenMP support
- Python 3 with venv
- IMDB CSV data at `/home/pei/Project/benchmarks/imdb_job-postgres/csv/` (~3.7 GB, 21 tables)
- Claude Code CLI with active subscription (`claude auth` to authenticate)

### Step 0: Verify Setup

```bash
cd /home/pei/Project/GenDB

# Confirm data exists
ls /home/pei/Project/benchmarks/imdb_job-postgres/csv/title.csv

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

### Step 1: Set Up Benchmark Directory

The script does this automatically. If doing it manually:

```bash
mkdir -p benchmarks/imdb-job/data benchmarks/imdb-job/query_results

# Copy schema (21 tables: name, title, cast_info, movie_info, etc.)
cp /home/pei/Project/benchmarks/imdb_job-postgres/schema.sql benchmarks/imdb-job/schema.sql

# Symlink CSV data (no scale factor for JOB — fixed IMDB snapshot, uses sf1 convention)
ln -s /home/pei/Project/benchmarks/imdb_job-postgres/csv benchmarks/imdb-job/data/sf1

# Concatenate 113 individual query files into one queries.sql with -- Q<id> headers
for f in $(ls /home/pei/Project/benchmarks/imdb_job-postgres/queries/*.sql | sort -V); do
  id="$(basename "${f%.sql}")"
  echo "-- Q${id}"
  cat "$f"
  echo ""
done > benchmarks/imdb-job/queries.sql
```

### Step 2: Generate Ground Truth

Ground truth lets GenDB verify correctness of generated C++ code against DuckDB results.
DuckDB is only needed for this step — the pipeline itself uses only Python stdlib for validation.

```bash
python3 -m venv benchmarks/imdb-job/.venv
benchmarks/imdb-job/.venv/bin/pip install duckdb==1.4.4 sqlglot
benchmarks/imdb-job/.venv/bin/python3 benchmarks/imdb-job/generate_ground_truth.py
```

Note: `sqlglot` is needed by GenDB's SQL template extractor (`src/gendb/tools/sql-parser.py`). The script activates the venv before running the pipeline so `python3` resolves to the venv python.

Expected: 113 CSV files in `benchmarks/imdb-job/query_results/`, all queries succeed.

### Step 3: Run GenDB Pipeline

The pipeline is identical to GenDB's TPC-H/SEC-EDGAR flow — same 5 agents, same prompts, same iteration loop. The only JOB-specific adaptation is the validation mode: JOB uses default sorted comparison (not `--tpch` positional or `--financial` tolerance), since JOB queries return simple MIN() aggregates without ORDER BY.

```bash
# Conservative first run
node src/gendb/orchestrator.mjs \
  --benchmark imdb-job \
  --sf 1 \
  --agent-provider claude-code \
  --max-concurrent 1 \
  --max-iterations 2

# Once validated, scale up
node src/gendb/orchestrator.mjs \
  --benchmark imdb-job \
  --sf 1 \
  --agent-provider claude-code \
  --max-concurrent 2 \
  --max-iterations 3
```

### Common Options

All defaults match GenDB's `gendb.config.mjs` (the same defaults used for TPC-H and SEC-EDGAR):

| Flag | Default | Source | Description |
|------|---------|--------|-------------|
| `--max-iterations N` | 5 | `gendb.config.mjs:16` | Max optimization iterations per query |
| `--max-concurrent N` | 22 | `gendb.config.mjs:18` | Max parallel queries (recommend 1–3 for claude-code) |
| `--stall-threshold N` | 5 | `gendb.config.mjs:17` | Stop after N non-improving iterations |
| `--optimization-target X` | hot | `gendb.config.mjs:15` | `hot` (avg of repeated runs) or `cold` (single run) |
| `--model M` | opus | `gendb.config.mjs:62` | Override model for all agents (opus, sonnet, haiku) |
| `--reoptimize <id\|all>` | — | — | Force re-optimization of specific or all queries |

---

## Pipeline Stages

GenDB runs the same 5-agent pipeline as for TPC-H and SEC-EDGAR:

1. **Workload Analyzer** — profiles hardware (CPU, cache, SIMD), samples CSV data, extracts join patterns and selectivity across all 113 queries
2. **Storage/Index Designer** — designs columnar binary storage with encoding (dict, fixed-int, compressed), indexes (hash, zone maps, bloom filters), generates and runs `ingest.cpp` + `build_indexes.cpp`
3. **Query Planner** — produces `plan.json` per query: join order, physical strategies, parallelism config
4. **Code Generator** — implements plan as optimized standalone C++ with `GENDB_PHASE` timing macros
5. **Query Optimizer** — iteratively refines: compiles, runs, profiles, diagnoses bottleneck, revises plan, regenerates code

Phase 1 (steps 1–2) runs once for the entire workload. Phase 2 (steps 3–5) runs per query, up to `--max-concurrent` in parallel.

Agent prompts are benchmark-agnostic — they receive the schema, queries, and data as context and generate optimized code for whatever workload is provided.

## Output Structure

```
output/imdb-job-sf1/
  workload_analysis.json
  storage/
    storage_design.json
    ingest/ingest.cpp
    ingest/build_indexes.cpp
    <table>.bin, *.idx, ...
  Q1a/
    iter0/
      plan.json
      Q1a.cpp
      Q1a              # compiled binary
      execution_results.json
      results.csv
    iter1/ ...
    best/              # symlink to best iteration
  Q1b/ ...
  runs/<run-id>/run.json
```

## Verifying Results

**Verify all queries at once:**

```bash
# Verify correctness of all binaries in the latest run against ground truth
bash verify_correctness.sh

# Or specify a particular run
bash verify_correctness.sh output/imdb-job-sf1/runs/2026-05-20T09-18-19
```

**Verify a single query manually:**

```bash
# Run the binary
./output/imdb-job-sf1/runs/latest/queries/Q1a/iter_0/q1a \
  output/imdb-job-sf1/storage /tmp/q1a_out

# Compare against ground truth (tool expects directories of CSVs)
python3 src/gendb/tools/compare_results.py \
  benchmarks/imdb-job/query_results \
  /tmp/q1a_out

# Timing breakdown
cat output/imdb-job-sf1/runs/latest/queries/Q1a/iter_0/execution_results.json
```

## Post-Pipeline: Verify and Measure

After the pipeline completes, run these steps separately. This keeps measurement decoupled from code generation — you can rerun measurements without rerunning the pipeline.

All three scripts **recompile all C++ sources before running**, so they work correctly even when binaries were compiled on a different machine. No manual recompilation needed.

### Compile Flags

For fair comparison with Bespoke, all scripts use the same optimization flags:

| System | Flags |
|--------|-------|
| GenDB | `g++ -O3 -march=native -std=c++17 -fopenmp -lpthread` |
| Bespoke | `g++ -O3 -march=native -std=c++20 -fPIC -flto` |

Both use `-O3 -march=native`. GenDB does **not** use `-DGENDB_PROFILE` for measurement (the orchestrator uses it during optimization for timing breakdown, but it adds printf overhead).

### Step 1: Verify Correctness

```bash
bash verify_correctness.sh
```

Recompiles all `best/` sources, then checks every query against DuckDB ground truth. Reports pass/fail per query. If any queries fail, rerun the pipeline (it only regenerates queries without a valid `best/`).

### Step 2: Measure Warm Execution

Recompiles, then measures query execution time only — no compilation, no data loading. Each pre-compiled binary runs against the persistent `output/imdb-job-sf1/storage/` directory. Pinned to core 3, 5 warmup + 10 measured runs.

```bash
python3 benchmarks/imdb-job/measure_gendb_warm.py
# Output: benchmarks/imdb-job/results/gendb_warm.csv
# Columns: query, median_ms, mean_ms, stddev_ms, run_1..run_10
```

### Step 3: Measure Cold Execution

Measures the full cold path per query: `g++ -O3` compilation + binary execution. Reports `cold_total`, `exec_only`, and `compile_only` separately (matching Bespoke's `bespoke_cold.csv` column format). GenDB compiles each query independently (~0.6–1.0s compile + execution time). Pinned to core 3, 5 warmup + 10 measured runs.

```bash
python3 benchmarks/imdb-job/measure_gendb_cold.py
# Output: benchmarks/imdb-job/results/gendb_cold.csv
# Columns: query, cold_median_ms, cold_mean_ms, cold_stddev_ms,
#          exec_median_ms, exec_mean_ms, exec_stddev_ms,
#          compile_median_ms, compile_mean_ms, run_1..run_10
```

### Output Files

All measurement CSVs use the same format as BespokeOLAP's benchmark suite for direct comparison:

| File | What it measures |
|------|-----------------|
| `gendb_warm.csv` | Execution only (pre-compiled binary) |
| `gendb_cold.csv` | Compile + execute (per-query g++ AOT compilation) |
| Bespoke `bespoke_warm.csv` | Execution only (JIT-compiled, all queries in one engine) |
| Bespoke `bespoke_cold.csv` | Compile + execute (shared ~29.5s JIT for all 113 queries) |
| `duckdb_threads1.csv` | DuckDB single-threaded baseline |
| `duckdb_parallel.csv` | DuckDB default parallel baseline |

## What We Changed for JOB

Summary of all modifications to GenDB for JOB benchmark support:

| File | Change |
|------|--------|
| `src/gendb/shared.mjs:82` | `parseQueryFile()` regex: `\d+` → `\w+` to handle alphanumeric query IDs (Q1a, Q33c) |
| `src/gendb/orchestrator.mjs:2473` | Validation: `--financial` only for `sec-edgar`, JOB uses default mode (sorted comparison) |
| `src/gendb/providers/claude-code.mjs` | New provider: spawns `claude -p` subprocess per agent |
| `src/gendb/providers/index.mjs` | Registered `claude-code` provider |
| `src/gendb/gendb.config.mjs` | Added `claude-code` provider config, documented `imdb-job` as valid benchmark |
| `benchmarks/imdb-job/` | Schema, concatenated queries, data symlink, ground truth generator |

No agent prompts were modified. The pipeline flow, iteration logic, and all parameters are identical to TPC-H/SEC-EDGAR runs.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| "Not logged in" | Run `claude auth` |
| Rate limiting / slow | Reduce `--max-concurrent` to 1. Close other Claude Code sessions. |
| Storage Designer timeout | Re-run — Phase 1 artifacts are persisted and reused automatically. The storage designer has a 45-minute timeout (`gendb.config.mjs:47`). |
| Wrong row counts after ingestion | Check generated `ingest.cpp` for CSV quote/escape handling. IMDB data has embedded commas in text fields (movie titles, character names). |
| Want to re-run everything | Use `--reoptimize all` to force re-optimization of all queries. |
