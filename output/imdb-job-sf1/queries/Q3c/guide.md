# Q3c Guide

## SQL
```sql
SELECT MIN(t.title) AS movie_title
FROM keyword AS k,
     movie_info AS mi,
     movie_keyword AS mk,
     title AS t
WHERE k.keyword LIKE '%sequel%'
  AND mi.info IN ('Sweden','Norway','Germany','Denmark',
                  'Swedish','Denish','Norwegian','German',
                  'USA','American')
  AND t.production_year > 1990
  AND t.id = mi.movie_id
  AND t.id = mk.movie_id
  AND mk.movie_id = mi.movie_id
  AND k.id = mk.keyword_id;
```

## Column Reference

### keyword.id (PK, int32_t)
- File: `keyword/id.bin` (rows = 134170); dense identity.

### keyword.keyword (varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat` (rows = 134170)
- This query's use: `LIKE '%sequel%'` → scan via `memmem` on `.dat` slice `[off[i], off[i+1])`; collect `seq_ids`.

### movie_info.movie_id (FK to title, int32_t)
- File: `movie_info/movie_id.bin` (rows = 14835720); FK-sorted by movie_id.

### movie_info.info (varlen)
- Files: `movie_info/info.off`, `movie_info/info.dat` (rows = 14835720)
- This query's use: `mi.info IN (10 literals)`. movie_info is FK-sorted by movie_id; scan the varlen column in row order of the FK-sorted child within candidate ranges. Build a `flat_hash_set<string_view>`; length prefilter on lengths {3,6,7,8,9} (USA=3; Sweden,Norway,German,Denish,Norway,USA variants are 3/6/7/8/9). No index on `movie_info.info`.

### movie_keyword.movie_id (FK to title, int32_t)
- File: `movie_keyword/movie_id.bin` (rows = 4523930); FK-sorted.

### movie_keyword.keyword_id (FK to keyword, int32_t)
- File: `movie_keyword/keyword_id.bin` (rows = 4523930)
- This query's use: equality with each kid in `seq_ids`; use `movie_keyword__keyword_id` CSR.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected `MIN(t.title)`.

### title.production_year (int32_t, nullable)
- File: `title/production_year.bin` (rows = 2528312); NULL = `INT32_MIN`.
- This query's use: `production_year > 1990`. C++: `y != INT32_MIN && y > 1990`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| keyword | 134,170 | dimension | id | 100000 |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: same as Q3a/b (star on title.id; `k ── mk.keyword_id`).
- Driver: collect `seq_ids` from keyword scan. Probe `movie_keyword__keyword_id` CSR per kid → candidate movie ids set. Filter candidates by `t.production_year > 1990` (most titles pass). For each surviving movie use `movie_info__movie_id` offsets_only to walk mi rows, test info against the IN-set (length prefilter then hash).
- Filter selectivities: `%sequel%` → ~tens of keyword ids → moderate mk slice; production_year > 1990 ≈ 50–60%; the IN-set is broader than Q3a (includes USA/American), but mi.info matches are still sparse per movie.
- Aggregation: `MIN(t.title)` → single output row.
- LIKE pattern: `%sequel%` on keyword.keyword via memmem on `.dat`.
- IN pattern: 10 literals; use `flat_hash_set<string_view>`; prune by length first.
- Output projection: read `t.title` varlen only for surviving rows.

## Indexes
- `movie_keyword__keyword_id` (CSR): `_idx/movie_keyword__keyword_id__offsets.bin` + `_idx/movie_keyword__keyword_id__rowids.bin`. Usage:
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
    /* test mi.info from info.off+.dat[r] against IN-set */
}
```

## Rules followed
- `%sequel%` via varlen scan only; no fabricated index on keyword text.
- `mi.info` is varlen on FK-sorted child; scanned in row order within candidate movie ranges; no claim of any index on it.
- Only declared CSR / offsets_only indexes used.
