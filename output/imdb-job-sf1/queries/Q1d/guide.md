# Q1d Guide

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
  AND t.production_year > 2000
  AND ct.id = mc.company_type_id
  AND t.id = mc.movie_id
  AND t.id = mi_idx.movie_id
  AND mc.movie_id = mi_idx.movie_id
  AND it.id = mi_idx.info_type_id;
```

## Column Reference

### company_type.id (PK, int32_t)
- File: `company_type/id.bin` (rows = 4); dense identity.
- This query's use: target id for `ct.kind='production companies'`.

### company_type.kind (varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat` (rows = 4)
- This query's use: scan to find `'production companies'` → `target_ct_id`.

### info_type.id (PK, int32_t)
- File: `info_type/id.bin` (rows = 113); dense identity.
- This query's use: target id for `it.info='bottom 10 rank'`.

### info_type.info (varlen)
- Files: `info_type/info.off`, `info_type/info.dat` (rows = 113)
- This query's use: scan to find `'bottom 10 rank'` → `target_it_id`.

### movie_companies.movie_id (FK, int32_t)
- File: `movie_companies/movie_id.bin` (rows = 2609129); FK-sorted.
- This query's use: join via `movie_companies__movie_id` offsets_only.

### movie_companies.company_type_id (FK, int32_t)
- File: `movie_companies/company_type_id.bin` (rows = 2609129)
- This query's use: `company_type_id_bin[r] == target_ct_id`.

### movie_companies.note (varlen)
- Files: `movie_companies/note.off`, `movie_companies/note.dat` (rows = 2609129)
- This query's use: NOT LIKE `'%(as Metro-Goldwyn-Mayer Pictures)%'` via `memmem`. Empty slice (NULL) fails NOT LIKE per SQL semantics → skip.
- Also projected as `MIN(mc.note)`.

### movie_info_idx.movie_id (FK, int32_t)
- File: `movie_info_idx/movie_id.bin` (rows = 1380035); FK-sorted.
- This query's use: join via `movie_info_idx__movie_id` offsets_only.

### movie_info_idx.info_type_id (FK, int32_t)
- File: `movie_info_idx/info_type_id.bin` (rows = 1380035)
- This query's use: `info_type_id_bin[r] == target_it_id`.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected `MIN(t.title)`.

### title.production_year (int32_t, nullable)
- File: `title/production_year.bin` (rows = 2528312); NULL `INT32_MIN`.
- This query's use: `production_year > 2000`. C++: `y != INT32_MIN && y > 2000`. Also projected `MIN`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_type | 4 | dimension | id | — |
| info_type | 113 | dimension | id | — |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |

## Query Analysis
- Join graph: star around `title.id`; mc and mi_idx are facts FK-sorted on movie_id.
- Driver: scan `title.production_year` once; for each title row i with year > 2000 (id = i+1), iterate mc range via offsets_only, then mi_idx range via offsets_only. Filter by `target_ct_id`, `target_it_id`, and NOT-LIKE note.
- Filter selectivities: production_year > 2000 ≈ 25–30% of titles; `bottom 10 rank` in mi_idx is extremely rare; ct kind narrows mc to ~25%; NOT LIKE on Metro-Goldwyn-Mayer is almost always true (but excludes NULLs).
- Aggregation: `MIN(...)` → single result row.
- LIKE: only the NOT LIKE; standard SQL semantics → empty (NULL) note rows fail.
- Output projection: read `mc.note`, `t.title`, `t.production_year` only for surviving rows.

## Indexes
- `movie_companies__movie_id` (offsets_only): `_idx/movie_companies__movie_id__offsets.bin`. Usage:
```cpp
int32_t lo = off[v]; int32_t hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* mc row r */ }
```
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`. Same pattern.

## Rules followed
- ct/it ids resolved at runtime; no hardcoded ids.
- No invented index on mc.note.
- Varlen accessed via `.off`+`.dat`.
