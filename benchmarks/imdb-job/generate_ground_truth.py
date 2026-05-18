#!/usr/bin/env python3
"""Generate ground truth query results for JOB (Join Order Benchmark) using DuckDB.

Reads schema.sql and queries.sql, loads CSV data, runs each query,
and saves results to benchmarks/imdb-job/query_results/Q<id>.csv.

Usage:
    python3 benchmarks/imdb-job/generate_ground_truth.py
    python3 benchmarks/imdb-job/generate_ground_truth.py --data-dir /path/to/csv
"""

import argparse
import csv
import os
import re
import sys

import duckdb


IMDB_TABLES = {
    "aka_name": "id INTEGER, person_id INTEGER, name TEXT, imdb_index VARCHAR(12), name_pcode_cf VARCHAR(5), name_pcode_nf VARCHAR(5), surname_pcode VARCHAR(5), md5sum VARCHAR(32)",
    "aka_title": "id INTEGER, movie_id INTEGER, title TEXT, imdb_index VARCHAR(12), kind_id INTEGER, production_year INTEGER, phonetic_code VARCHAR(5), episode_of_id INTEGER, season_nr INTEGER, episode_nr INTEGER, note TEXT, md5sum VARCHAR(32)",
    "cast_info": "id INTEGER, person_id INTEGER, movie_id INTEGER, person_role_id INTEGER, note TEXT, nr_order INTEGER, role_id INTEGER",
    "char_name": "id INTEGER, name TEXT, imdb_index VARCHAR(12), imdb_id INTEGER, name_pcode_nf VARCHAR(5), surname_pcode VARCHAR(5), md5sum VARCHAR(32)",
    "comp_cast_type": "id INTEGER, kind VARCHAR(32)",
    "company_name": "id INTEGER, name TEXT, country_code VARCHAR(255), imdb_id INTEGER, name_pcode_nf VARCHAR(5), name_pcode_sf VARCHAR(5), md5sum VARCHAR(32)",
    "company_type": "id INTEGER, kind VARCHAR(32)",
    "complete_cast": "id INTEGER, movie_id INTEGER, subject_id INTEGER, status_id INTEGER",
    "info_type": "id INTEGER, info VARCHAR(32)",
    "keyword": "id INTEGER, keyword TEXT, phonetic_code VARCHAR(5)",
    "kind_type": "id INTEGER, kind VARCHAR(15)",
    "link_type": "id INTEGER, link VARCHAR(32)",
    "movie_companies": "id INTEGER, movie_id INTEGER, company_id INTEGER, company_type_id INTEGER, note TEXT",
    "movie_info": "id INTEGER, movie_id INTEGER, info_type_id INTEGER, info TEXT, note TEXT",
    "movie_info_idx": "id INTEGER, movie_id INTEGER, info_type_id INTEGER, info TEXT, note TEXT",
    "movie_keyword": "id INTEGER, movie_id INTEGER, keyword_id INTEGER",
    "movie_link": "id INTEGER, movie_id INTEGER, linked_movie_id INTEGER, link_type_id INTEGER",
    "name": "id INTEGER, name TEXT, imdb_index VARCHAR(12), imdb_id INTEGER, gender VARCHAR(1), name_pcode_cf VARCHAR(5), name_pcode_nf VARCHAR(5), surname_pcode VARCHAR(5), md5sum VARCHAR(32)",
    "person_info": "id INTEGER, person_id INTEGER, info_type_id INTEGER, info TEXT, note TEXT",
    "role_type": "id INTEGER, role VARCHAR(32)",
    "title": "id INTEGER, title TEXT, imdb_index VARCHAR(12), kind_id INTEGER, production_year INTEGER, imdb_id INTEGER, phonetic_code VARCHAR(5), episode_of_id INTEGER, season_nr INTEGER, episode_nr INTEGER, series_years VARCHAR(49), md5sum VARCHAR(32)",
}


def parse_queries(queries_path):
    """Parse queries.sql into a dict of {query_name: sql}."""
    with open(queries_path, "r") as f:
        content = f.read()

    queries = {}
    parts = re.split(r"(--\s*Q\w+)\s*\n", content)
    i = 1
    while i + 1 <= len(parts):
        name_match = re.match(r"--\s*(Q\w+)", parts[i])
        if name_match:
            name = name_match.group(1)
            sql = parts[i + 1].strip()
            sql = re.sub(r"--[^\n]*$", "", sql, flags=re.MULTILINE).strip()
            if sql and not sql.endswith(";"):
                sql += ";"
            if sql:
                queries[name] = sql
        i += 2

    return queries


def natural_sort_key(name):
    """Sort Q1a, Q1b, ..., Q2a, ..., Q10a, Q10b, ..., Q33c naturally."""
    m = re.match(r"Q(\d+)([a-z]*)", name)
    if m:
        return (int(m.group(1)), m.group(2))
    return (0, name)


def load_imdb_data(con, data_dir):
    """Load IMDB CSV data into DuckDB."""
    for table, columns in IMDB_TABLES.items():
        csv_path = os.path.join(data_dir, f"{table}.csv")
        if not os.path.exists(csv_path):
            print(f"  Warning: {csv_path} not found, skipping {table}")
            continue
        print(f"  Loading {table} from {csv_path}...")
        con.execute(f"CREATE TABLE {table} ({columns})")
        con.execute(
            f"COPY {table} FROM '{csv_path}' (DELIMITER ',', HEADER false, QUOTE '\"', ESCAPE '\\', NULL '', IGNORE_ERRORS true)"
        )
        count = con.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
        print(f"    {table}: {count:,} rows")


def main():
    parser = argparse.ArgumentParser(description="Generate JOB ground truth results")
    parser.add_argument("--data-dir", type=str, help="Path to CSV files directory")
    parser.add_argument("--output-dir", type=str, help="Output directory for query results")
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    queries_path = os.path.join(script_dir, "queries.sql")

    if args.data_dir:
        data_dir = args.data_dir
    else:
        data_dir = os.path.join(script_dir, "data", "sf1")

    if args.output_dir:
        output_dir = args.output_dir
    else:
        output_dir = os.path.join(script_dir, "query_results")

    if not os.path.exists(data_dir):
        print(f"Error: data directory not found: {data_dir}")
        sys.exit(1)

    os.makedirs(output_dir, exist_ok=True)

    print(f"Data directory: {data_dir}")
    print(f"Output directory: {output_dir}")

    con = duckdb.connect(":memory:")
    print("\nLoading IMDB data...")
    load_imdb_data(con, data_dir)

    queries = parse_queries(queries_path)
    sorted_names = sorted(queries.keys(), key=natural_sort_key)
    print(f"\nFound {len(queries)} queries: {', '.join(sorted_names)}")

    success = 0
    errors = 0
    for name in sorted_names:
        sql = queries[name]
        print(f"\nRunning {name}...")
        try:
            result = con.execute(sql)
            columns = [desc[0] for desc in result.description]
            rows = result.fetchall()

            output_path = os.path.join(output_dir, f"{name}.csv")
            with open(output_path, "w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(columns)
                for row in rows:
                    processed = []
                    for val in row:
                        if isinstance(val, float):
                            processed.append(f"{val:.2f}")
                        elif val is None:
                            processed.append("")
                        else:
                            processed.append(str(val))
                    writer.writerow(processed)

            print(f"  {name}: {len(rows)} rows -> {output_path}")
            success += 1
        except Exception as e:
            print(f"  {name}: ERROR - {e}")
            errors += 1

    con.close()
    print(f"\nGround truth: {success} succeeded, {errors} failed")
    print(f"Output directory: {output_dir}")


if __name__ == "__main__":
    main()
