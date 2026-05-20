# Q4a Guide

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
  AND mi_idx.info > '5.0'
  AND t.production_year > 2005
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
- This query's use: `info_type_id_bin[r] == target_it_id`. Alternative: use `movie_info_idx__info_type_id` CSR to fetch only rating rows directly.

### movie_info_idx.info (varlen)
- Files: `movie_info_idx/info.off`, `movie_info_idx/info.dat` (rows = 1380035)
- This query's use: string comparison `mi_idx.info > '5.0'`. movie_info_idx is FK-sorted by movie_id; scan the varlen column in row order of the FK-sorted child (for the candidate movie range, or via the info_type CSR rowid). Use `std::string_view::operator>` (lexicographic). Empty slice (NULL) treated as failing.
- Also projected via `MIN(mi_idx.info)` — read same bytes for surviving rows.

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
- This query's use: `production_year > 2005`. C++: `y != INT32_MIN && y > 2005`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| info_type | 113 | dimension | id | — |
| keyword | 134,170 | dimension | id | 100000 |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |

## Query Analysis
- Join graph: `it ── mi_idx.info_type_id`; `k ── mk.keyword_id`; `mk.movie_id = mi_idx.movie_id = t.id` (star on title.id).
- Driver: resolve `target_it_id` for `'rating'` and `seq_ids` for `%sequel%`. Probe `movie_keyword__keyword_id` CSR per kid → candidate movie set. Filter candidates by `t.production_year > 2005`. For each surviving movie, fetch mi_idx range via `movie_info_idx__movie_id`; require `info_type_id == target_it_id` and `info > "5.0"`. Maintain running min of `mi_idx.info` (lex) and `t.title` (lex).
- Filter selectivities: `it.info='rating'` extremely selective in mi_idx; `%sequel%` keyword small set; production_year > 2005 ~15–20%; lex `> '5.0'` over ratings keeps roughly half of ratings.
- Aggregation: `MIN(mi_idx.info), MIN(t.title)` → single output row.
- LIKE pattern: `%sequel%` on `keyword.keyword` via memmem; no text index on keyword.
- String comparison `> '5.0'` is lexicographic on the raw varlen bytes (not numeric); since ratings are formatted as e.g. `"4.5"`, `"10"`, this is the JOB-standard semantics.
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
- `target_it_id` for `'rating'` resolved at runtime by scanning `info_type/info.dat`; never hardcoded.
- `%sequel%` evaluated by varlen scan; no fabricated index on keyword text.
- `mi_idx.info` is varlen on FK-sorted child; scanned in row order within candidate movie ranges; no claim of any index on it.
- Only declared offsets_only / CSR indexes used.
