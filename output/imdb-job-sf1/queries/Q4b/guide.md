# Q4b Guide

## SQL
```sql
SELECT MIN(mi_idx.info) AS rating,
       MIN(t.title) AS movie_title
FROM info_type AS it,
     keyword AS k,
     movie_info_idx AS mi_idx,
     movie_keyword AS mk,
     title AS t
WHERE it.info ='rating'
  AND k.keyword LIKE '%sequel%'
  AND mi_idx.info > '9.0'
  AND t.production_year > 2010
  AND t.id = mi_idx.movie_id
  AND t.id = mk.movie_id
  AND mk.movie_id = mi_idx.movie_id
  AND k.id = mk.keyword_id
  AND it.id = mi_idx.info_type_id;
```

## Column Reference

### info_type.id (PK, int32_t)
- File: `info_type/id.bin` (rows = 113); dense identity.
- This query's use: target id for `it.info='rating'`.

### info_type.info (varlen)
- Files: `info_type/info.off`, `info_type/info.dat` (rows = 113)
- This query's use: scan to resolve `'rating'` → `target_it_id`.

### keyword.id (PK, int32_t)
- File: `keyword/id.bin` (rows = 134170); dense identity.

### keyword.keyword (varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat` (rows = 134170)
- This query's use: `LIKE '%sequel%'` → scan via `memmem` on `.dat`. Collect `seq_ids`.

### movie_info_idx.movie_id (FK to title, int32_t)
- File: `movie_info_idx/movie_id.bin` (rows = 1380035); FK-sorted by movie_id.
- This query's use: join with `t.id` via `movie_info_idx__movie_id` offsets_only.

### movie_info_idx.info_type_id (FK to info_type, int32_t)
- File: `movie_info_idx/info_type_id.bin` (rows = 1380035)
- This query's use: `info_type_id_bin[r] == target_it_id`.

### movie_info_idx.info (varlen)
- Files: `movie_info_idx/info.off`, `movie_info_idx/info.dat` (rows = 1380035)
- This query's use: lex compare `mi_idx.info > '9.0'`. movie_info_idx is FK-sorted by movie_id; scan the varlen column in row order of the FK-sorted child within candidate ranges. Empty (NULL) fails. Also projected via `MIN(mi_idx.info)`.

### movie_keyword.movie_id (FK to title, int32_t)
- File: `movie_keyword/movie_id.bin` (rows = 4523930); FK-sorted.

### movie_keyword.keyword_id (FK to keyword, int32_t)
- File: `movie_keyword/keyword_id.bin` (rows = 4523930)
- This query's use: equality with each kid in `seq_ids` via `movie_keyword__keyword_id` CSR.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected `MIN(t.title)`.

### title.production_year (int32_t, nullable)
- File: `title/production_year.bin` (rows = 2528312); NULL = `INT32_MIN`.
- This query's use: `production_year > 2010`. C++: `y != INT32_MIN && y > 2010`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| info_type | 113 | dimension | id | — |
| keyword | 134,170 | dimension | id | 100000 |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |

## Query Analysis
- Join graph: `it ── mi_idx.info_type_id`; `k ── mk.keyword_id`; `mk.movie_id = mi_idx.movie_id = t.id`.
- Driver: resolve `target_it_id` and `seq_ids`. Probe `movie_keyword__keyword_id` CSR per kid → candidate movie set. Filter by `t.production_year > 2010`. For each surviving movie, fetch mi_idx range; require `info_type_id == target_it_id` and lex `info > "9.0"`.
- Filter selectivities: `> '9.0'` lex very selective on ratings (≈ top 10% of ratings); production_year > 2010 ≈ 10–12%; `%sequel%` selects small mk slice; rating it_id is selective in mi_idx.
- Aggregation: `MIN(mi_idx.info), MIN(t.title)` → single result row.
- LIKE pattern: `%sequel%` via memmem; no text index.
- String comparison `> '9.0'` is lexicographic over varlen bytes (JOB-standard).
- Output projection: read `mi_idx.info` and `t.title` varlen only for surviving rows.

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
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`. Usage:
```cpp
int32_t lo = off[mv]; int32_t hi = off[mv+1];
for (int32_t r = lo; r < hi; ++r) { /* mi_idx row r */ }
```
- `movie_info_idx__info_type_id` (CSR): `_idx/movie_info_idx__info_type_id__offsets.bin` + `_idx/movie_info_idx__info_type_id__rowids.bin`. Optional alternative driver: enumerate all mi_idx rows for `target_it_id` directly.

## Rules followed
- `target_it_id` resolved at runtime; never hardcoded.
- `%sequel%` evaluated by varlen scan; no fabricated text index.
- `mi_idx.info` is varlen on FK-sorted child; scanned in row order within candidate ranges.
- Only declared CSR / offsets_only indexes used.
