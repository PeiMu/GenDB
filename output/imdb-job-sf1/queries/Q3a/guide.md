# Q3a Guide

## SQL
```sql
SELECT MIN(t.title) AS movie_title
FROM keyword AS k,
     movie_info AS mi,
     movie_keyword AS mk,
     title AS t
WHERE k.keyword LIKE '%sequel%'
  AND mi.info IN ('Sweden','Norway','Germany','Denmark',
                  'Swedish','Denish','Norwegian','German')
  AND t.production_year > 2005
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND mk.movie_id = mi.movie_id
  AND k.id = mk.keyword_id;
```

## Column Reference

### keyword.id (PK, int32_t)
- File: `keyword/id.bin` (rows = 134170); dense identity, row i ↔ id (i+1).
- This query's use: join with `mk.keyword_id`.

### keyword.keyword (varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat` (rows = 134170)
- This query's use: `LIKE '%sequel%'` → scan via `memmem` on `.dat` slice `[off[i], off[i+1])`. Length prefilter: skip i if `(off[i+1]-off[i]) < 6`. Collect matching ids into `seq_ids` set (small, ~tens).

### movie_info.movie_id (FK to title, int32_t)
- File: `movie_info/movie_id.bin` (rows = 14835720); FK-sorted by movie_id.
- This query's use: join with `t.id` via `movie_info__movie_id` offsets_only.

### movie_info.info (varlen)
- Files: `movie_info/info.off`, `movie_info/info.dat` (rows = 14835720)
- This query's use: `mi.info IN ('Sweden','Norway','Germany','Denmark','Swedish','Denish','Norwegian','German')`. movie_info is FK-sorted by movie_id; scan the varlen column in row order of the FK-sorted child (for the candidate movie range). Build a `flat_hash_set<string_view>` of the 8 literals; prune by length first (lengths 6,6,7,7,7,6,9,6) — only check rows whose length is in {6,7,9}. (Note: there is no index on `movie_info.info`.)

### movie_keyword.movie_id (FK to title, int32_t)
- File: `movie_keyword/movie_id.bin` (rows = 4523930); FK-sorted by movie_id.
- This query's use: join with `t.id` via `movie_keyword__movie_id` offsets_only.

### movie_keyword.keyword_id (FK to keyword, int32_t)
- File: `movie_keyword/keyword_id.bin` (rows = 4523930)
- This query's use: `keyword_id_bin[r] ∈ seq_ids`. Alternative: union of `movie_keyword__keyword_id` CSR ranges over each seq_id to collect candidate movie_id set directly.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected `MIN(t.title)`.

### title.production_year (int32_t, nullable)
- File: `title/production_year.bin` (rows = 2528312); NULL = `INT32_MIN`.
- This query's use: `production_year > 2005`. C++: `y != INT32_MIN && y > 2005`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| keyword | 134,170 | dimension | id | 100000 |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: `k ── mk.keyword_id`, `mk.movie_id = mi.movie_id = t.id`.
- Driver: collect `seq_ids` (keyword ids matching `%sequel%`). For each `kid ∈ seq_ids`, walk `movie_keyword__keyword_id` CSR to get mk rowids; collect distinct candidate movie ids in a sorted set / bitset. Iterate candidate movies in ascending order, apply `t.production_year > 2005`, then for each surviving movie use `movie_info__movie_id` offsets_only to fetch mi rows and test `mi.info` against the country/language IN-set.
- Filter selectivities: `%sequel%` matches ~tens of keywords → small mk slice; production_year > 2005 selects ~15–20% of titles; the IN-list is a tiny set of nations/languages — fraction of mi rows is small but mi has 14.8M rows so absolute scan cost only matters within candidate movie ranges.
- Aggregation: `MIN(t.title)` → single output row.
- LIKE pattern: `%sequel%` against `keyword.keyword` varlen — full varlen scan via `memmem`; no index on keyword text.
- IN pattern: build `flat_hash_set<string_view>` of 8 literals; per-row length prefilter; on match break out of mi inner loop (existence semantics).
- Output projection: read `t.title` varlen only for surviving rows.

## Indexes
- `movie_keyword__keyword_id` (CSR): `_idx/movie_keyword__keyword_id__offsets.bin` (int32, 134172) + `_idx/movie_keyword__keyword_id__rowids.bin` (int32, 4523930). Usage:
```cpp
for (int32_t kid : seq_ids) {
    int32_t lo = off[kid]; int32_t hi = off[kid + 1];
    for (int32_t k_pos = lo; k_pos < hi; ++k_pos) {
        int32_t mk_row = rowids[k_pos];
        int32_t mv = movie_id_bin[mk_row];
        candidate_movies.insert(mv);
    }
}
```
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin`. Usage:
```cpp
int32_t lo = off[mv]; int32_t hi = off[mv+1];
for (int32_t r = lo; r < hi; ++r) {
    /* read mi.info via info.off+.dat[r] and test IN-set */
}
```

## Rules followed
- `%sequel%` evaluated via varlen scan only; no claim of any index on keyword text.
- `mi.info` is FK-sorted child varlen; scanned in row order of the FK-sorted child within the candidate movie range — no claim of any index on it.
- Only declared CSR / offsets_only indexes used.
