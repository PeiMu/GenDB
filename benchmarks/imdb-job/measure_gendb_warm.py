"""
Measure GenDB JOB query execution time (warm, pre-compiled binaries).
- Recompiles all C++ sources first (ensures binary compatibility)
- 5 warmup + 10 measured runs per query
- Pin to core 3 (matching DuckDB/Bespoke measurements)

Usage:
    python benchmarks/imdb-job/measure_gendb_warm.py
"""
import csv
import os
import statistics
import subprocess
import time
from pathlib import Path

BENCHMARK_DIR = Path(__file__).resolve().parent
GENDB_ROOT = BENCHMARK_DIR.parent.parent
WORKLOAD_DIR = GENDB_ROOT / "output" / "imdb-job-sf1"
STORAGE = WORKLOAD_DIR / "storage"
INCLUDE_DIR = GENDB_ROOT / "src" / "gendb" / "utils"
RESULTS_DIR = BENCHMARK_DIR / "results"

CXX_FLAGS = ["-O3", "-march=native", "-std=c++17", "-fopenmp", "-lpthread"]

WARMUP_RUNS = 5
MEASURED_RUNS = 10
PIN_CORE = 3

ALL_QUERY_IDS = [
    "1a","1b","1c","1d",
    "2a","2b","2c","2d",
    "3a","3b","3c",
    "4a","4b","4c",
    "5a","5b","5c",
    "6a","6b","6c","6d","6e","6f",
    "7a","7b","7c",
    "8a","8b","8c","8d",
    "9a","9b","9c","9d",
    "10a","10b","10c",
    "11a","11b","11c","11d",
    "12a","12b","12c",
    "13a","13b","13c","13d",
    "14a","14b","14c",
    "15a","15b","15c","15d",
    "16a","16b","16c","16d",
    "17a","17b","17c","17d","17e","17f",
    "18a","18b","18c",
    "19a","19b","19c","19d",
    "20a","20b","20c",
    "21a","21b","21c",
    "22a","22b","22c","22d",
    "23a","23b","23c",
    "24a","24b",
    "25a","25b","25c",
    "26a","26b","26c",
    "27a","27b","27c",
    "28a","28b","28c",
    "29a","29b","29c",
    "30a","30b","30c",
    "31a","31b","31c",
    "32a","32b",
    "33a","33b","33c",
]


def get_cpp_and_bin(qname):
    qid = f"Q{qname}"
    best_dir = WORKLOAD_DIR / "queries" / qid / "best"
    return best_dir / f"q{qname}.cpp", best_dir / f"q{qname}"


def recompile_all():
    print("=== Recompiling all queries ===")
    compiled = 0
    failed = []
    for qname in ALL_QUERY_IDS:
        cpp, binary = get_cpp_and_bin(qname)
        if not cpp.exists():
            failed.append(qname)
            continue
        rc = subprocess.run(
            ["g++"] + CXX_FLAGS + [f"-I{INCLUDE_DIR}", str(cpp), "-o", str(binary)],
            capture_output=True,
        ).returncode
        if rc == 0:
            compiled += 1
        else:
            failed.append(qname)
            print(f"  {qname}: COMPILE ERROR")
    print(f"Compiled: {compiled}/{compiled + len(failed)}")
    if failed:
        print(f"Failed: {', '.join(failed)}")
    print()
    return failed


def run_query(binary, tmp_dir):
    start = time.perf_counter()
    proc = subprocess.run(
        [str(binary), str(STORAGE), str(tmp_dir)],
        capture_output=True, timeout=120,
    )
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    return elapsed_ms, proc.returncode


def main():
    compile_failures = recompile_all()

    os.sched_setaffinity(0, {PIN_CORE})
    print(f"Pinned to core {PIN_CORE}")
    print(f"Storage: {STORAGE}")
    print(f"Warmup: {WARMUP_RUNS}, Measured: {MEASURED_RUNS}")

    tmp_dir = Path("/tmp/gendb_bench")
    tmp_dir.mkdir(exist_ok=True)

    results = []
    skipped = []

    for qname in ALL_QUERY_IDS:
        _, binary = get_cpp_and_bin(qname)
        if not binary.exists() or not os.access(binary, os.X_OK):
            skipped.append(qname)
            continue

        for _ in range(WARMUP_RUNS):
            run_query(binary, tmp_dir)

        timings = []
        for _ in range(MEASURED_RUNS):
            t, rc = run_query(binary, tmp_dir)
            if rc != 0:
                break
            timings.append(t)

        if len(timings) < MEASURED_RUNS:
            print(f"  {qname}: RUNTIME ERROR (got {len(timings)}/{MEASURED_RUNS} runs)")
            skipped.append(qname)
            continue

        median_ms = statistics.median(timings)
        mean_ms = statistics.mean(timings)
        stddev_ms = statistics.stdev(timings) if len(timings) > 1 else 0.0

        results.append({
            "query": qname,
            "median_ms": round(median_ms, 3),
            "mean_ms": round(mean_ms, 3),
            "stddev_ms": round(stddev_ms, 3),
            "runs": timings,
        })
        print(f"  {qname}: median={median_ms:.3f}ms mean={mean_ms:.3f}ms stddev={stddev_ms:.3f}ms")

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    output_path = RESULTS_DIR / "gendb_warm.csv"

    with open(output_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["query", "median_ms", "mean_ms", "stddev_ms"] +
                        [f"run_{i+1}" for i in range(MEASURED_RUNS)])
        for r in results:
            writer.writerow([r["query"], r["median_ms"], r["mean_ms"], r["stddev_ms"]] +
                            [round(t, 3) for t in r["runs"]])

    print(f"\nResults: {output_path}")
    print(f"Measured: {len(results)}/{len(ALL_QUERY_IDS)}")
    if skipped:
        print(f"Skipped: {', '.join(skipped)}")


if __name__ == "__main__":
    main()
