"""
Measure GenDB JOB cold execution: g++ compilation + binary execution per query.
- GenDB compiles each query independently (AOT, ~0.6-1.0s per query)
- 5 warmup + 10 measured runs
- Reports cold_total, exec_only, and compile_only separately (matching Bespoke cold format)
- Pinned to core 3

Usage:
    python benchmarks/imdb-job/measure_gendb_cold.py
"""
import csv
import os
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

BENCHMARK_DIR = Path(__file__).resolve().parent
GENDB_ROOT = BENCHMARK_DIR.parent.parent
WORKLOAD_DIR = GENDB_ROOT / "output" / "imdb-job-sf1"
STORAGE = WORKLOAD_DIR / "storage"
INCLUDE_DIR = GENDB_ROOT / "src" / "gendb" / "utils"
RESULTS_DIR = BENCHMARK_DIR / "results"

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


def find_cpp(qname):
    qid = f"Q{qname}"
    best_dir = WORKLOAD_DIR / "queries" / qid / "best"
    cpp = best_dir / f"q{qname}.cpp"
    if cpp.exists():
        return cpp
    return None


CXX_FLAGS = ["-O3", "-march=native", "-std=c++17", "-fopenmp", "-lpthread"]


def cold_run(cpp_path, output_binary, result_dir):
    """Compile then execute. Returns (compile_ms, exec_ms, ok)."""
    # Compile
    t0 = time.perf_counter()
    rc = subprocess.run(
        ["g++"] + CXX_FLAGS + [f"-I{INCLUDE_DIR}", str(cpp_path), "-o", str(output_binary)],
        capture_output=True, timeout=60,
    ).returncode
    compile_ms = (time.perf_counter() - t0) * 1000.0
    if rc != 0:
        return compile_ms, 0.0, False

    # Execute
    t1 = time.perf_counter()
    rc = subprocess.run(
        [str(output_binary), str(STORAGE), str(result_dir)],
        capture_output=True, timeout=120,
    ).returncode
    exec_ms = (time.perf_counter() - t1) * 1000.0
    return compile_ms, exec_ms, rc == 0


def main():
    os.sched_setaffinity(0, {PIN_CORE})
    print(f"GenDB Cold Benchmark (compile + execute per query)")
    print(f"Pinned to core {PIN_CORE}")
    print(f"Warmup: {WARMUP_RUNS}, Measured: {MEASURED_RUNS}")

    results = []
    skipped = []

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)

        for qname in ALL_QUERY_IDS:
            cpp = find_cpp(qname)
            if cpp is None:
                skipped.append(qname)
                continue

            out_bin = tmpdir / f"q{qname}"
            res_dir = tmpdir / f"res_{qname}"
            res_dir.mkdir(exist_ok=True)

            # Warmup
            for _ in range(WARMUP_RUNS):
                cold_run(cpp, out_bin, res_dir)

            # Measured runs
            compile_times = []
            exec_times = []
            cold_times = []
            ok = True
            for _ in range(MEASURED_RUNS):
                c_ms, e_ms, success = cold_run(cpp, out_bin, res_dir)
                if not success:
                    ok = False
                    break
                compile_times.append(c_ms)
                exec_times.append(e_ms)
                cold_times.append(c_ms + e_ms)

            if not ok or len(cold_times) < MEASURED_RUNS:
                print(f"  {qname}: ERROR")
                skipped.append(qname)
                continue

            results.append({
                "query": qname,
                "cold_median": statistics.median(cold_times),
                "cold_mean": statistics.mean(cold_times),
                "cold_stddev": statistics.stdev(cold_times),
                "exec_median": statistics.median(exec_times),
                "exec_mean": statistics.mean(exec_times),
                "exec_stddev": statistics.stdev(exec_times),
                "compile_median": statistics.median(compile_times),
                "compile_mean": statistics.mean(compile_times),
                "cold_runs": cold_times,
            })
            print(f"  {qname}: cold={statistics.median(cold_times):.1f}ms "
                  f"(compile={statistics.median(compile_times):.1f}ms + "
                  f"exec={statistics.median(exec_times):.1f}ms)")

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    output_path = RESULTS_DIR / "gendb_cold.csv"

    with open(output_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["query",
                         "cold_median_ms", "cold_mean_ms", "cold_stddev_ms",
                         "exec_median_ms", "exec_mean_ms", "exec_stddev_ms",
                         "compile_median_ms", "compile_mean_ms"] +
                        [f"run_{i+1}" for i in range(MEASURED_RUNS)])
        for r in results:
            writer.writerow([r["query"],
                             round(r["cold_median"], 3),
                             round(r["cold_mean"], 3),
                             round(r["cold_stddev"], 3),
                             round(r["exec_median"], 3),
                             round(r["exec_mean"], 3),
                             round(r["exec_stddev"], 3),
                             round(r["compile_median"], 3),
                             round(r["compile_mean"], 3)] +
                            [round(t, 3) for t in r["cold_runs"]])

    print(f"\nResults: {output_path}")
    print(f"Measured: {len(results)}/{len(ALL_QUERY_IDS)}")
    if skipped:
        print(f"Skipped: {', '.join(skipped)}")


if __name__ == "__main__":
    main()
