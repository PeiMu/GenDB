# Q1b Guide

## SQL
```sql
SELECT MIN(mc.note) AS production_note,
       MIN(t.title) AS movie_title,
       MIN(t.production_year) AS movie_year
FROM company_type AS ct,
     info_type AS it,
     movie_companies AS mc,
     movie_info_idx AS mi_idx,
     title AS t
WHERE ct.kind = 'production companies'
  AND it.info = 'bottom 10 rank'
  AND mc.note NOT LIKE '%(as Metro-Goldwyn-Mayer Pictures)%'
  AND t.production_year BETWEEN 2005 AND 2010
  AND ct.id = mc.company_type_id
  AND t.id = mc.movie_id
  AND t.id = mi_idx.movie_id
  AND mc.movie_id = mi_idx.movie_id
  AND it.id = mi_idx.info_type_id;
```

## Column Reference

### company_type.id (PK, int32_t)
- File: `company_type/id.bin` (rows = 4); dense identity.
- This query's use: target for `ct.kind='production companies'`.

### company_type.kind (dimension text, varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat` (rows = 4)
- This query's use: scan to resolve `'production companies'` → `target_ct_id`.

### info_type.id (PK, int32_t)
- File: `info_type/id.bin` (rows = 113); dense identity.
- This query's use: target for `it.info='bottom 10 rank'`.

### info_type.info (dimension text, varlen)
- Files: `info_type/info.off`, `info_type/info.dat` (rows = 113)
- This query's use: scan to resolve `'bottom 10 rank'` → `target_it_id`.

### movie_companies.movie_id (FK, int32_t)
- File: `movie_companies/movie_id.bin` (rows = 2609129); FK-sorted by movie_id.
- This query's use: join with `t.id` via `movie_companies__movie_id` offsets_only.

### movie_companies.company_type_id (FK, int32_t)
- File: `movie_companies/company_type_id.bin` (rows = 2609129)
- This query's use: `company_type_id_bin[r] == target_ct_id`.

### movie_companies.note (varlen)
- Files: `movie_companies/note.off`, `movie_companies/note.dat` (rows = 2609129)
- This query's use: NOT LIKE `'%(as Metro-Goldwyn-Mayer Pictures)%'` → `memmem` over `[off[r], off[r+1])`. Empty slice (NULL) does NOT satisfy NOT LIKE in SQL semantics; here we treat empty slice as failing (skip).
- Also projected as `MIN(mc.note)`.

### movie_info_idx.movie_id (FK, int32_t)
- File: `movie_info_idx/movie_id.bin` (rows = 1380035); FK-sorted.
- This query's use: join with `t.id` via `movie_info_idx__movie_id` offsets_only.

### movie_info_idx.info_type_id (FK, int32_t)
- File: `movie_info_idx/info_type_id.bin` (rows = 1380035)
- This query's use: `info_type_id_bin[r] == target_it_id`.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity.
- This query's use: driver / join key.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected via `MIN(t.title)`.

### title.production_year (int32_t, nullable)
- File: `title/production_year.bin` (rows = 2528312); NULLs `INT32_MIN`.
- This query's use: predicate `BETWEEN 2005 AND 2010`. C++: `int32_t y = production_year_bin[i]; y != INT32_MIN && y >= 2005 && y <= 2010`. Also projected as `MIN(t.production_year)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_type | 4 | dimension | id | — |
| info_type | 113 | dimension | id | — |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |

## Query Analysis
- Join graph: same as Q1a (star around `title`). `mc.movie_id = mi_idx.movie_id` is implied by both joining `t.id`.
- Driver: scan `title.production_year.bin` once to find years in [2005, 2010] (~6 of ~135 year buckets ≈ moderate selectivity). For each surviving title row i (id = i+1), use both offsets_only indexes to fetch mc and mi_idx ranges.
- Filter selectivities: production_year window ~5-8%; `it.info='bottom 10 rank'` is rare (it_id-equality wipes nearly all of mi_idx); `ct.kind='production companies'` selects ~25% of mc rows; `mc.note NOT LIKE '%(as Metro-Goldwyn-Mayer Pictures)%'` is almost always true.
- Aggregation: `MIN(...)` → single output row; maintain three running mins, skipping NULLs.
- LIKE notes: only a NOT LIKE in Q1b — for empty-string note rows treat as failing (NULL semantics).
- Output projection: read `mc.note`, `t.title`, `t.production_year` only for surviving rows.

## Indexes
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`. Query-time:
```cpp
int32_t lo = off[v]; int32_t hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* mc row r */ }
```
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`. Same usage pattern.

## Rules followed
- ct/it ids resolved by scanning dense-PK varlen columns at runtime.
- No fabricated indexes; only offsets_only on movie_id used.
- mc.note treated as varlen via `.off`+`.dat`; no claim of any text index.
