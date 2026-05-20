# Q1a Guide

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
  AND it.info = 'top 250 rank'
  AND mc.note NOT LIKE '%(as Metro-Goldwyn-Mayer Pictures)%'
  AND (mc.note LIKE '%(co-production)%'
       OR mc.note LIKE '%(presents)%')
  AND ct.id = mc.company_type_id
  AND t.id = mc.movie_id
  AND t.id = mi_idx.movie_id
  AND mc.movie_id = mi_idx.movie_id
  AND it.id = mi_idx.info_type_id;
```

## Column Reference

### company_type.id (PK, int32_t)
- File: `company_type/id.bin` (rows = 4)
- `id_is_dense_1_to_N` so row i ↔ id (i+1). Identity array.
- This query's use: target for `ct.kind = 'production companies'` lookup.

### company_type.kind (dimension text, varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat` (rows = 4)
- This query's use: scan to resolve the literal `'production companies'` → `target_ct_id` (see shared context's id↔row-index identity pattern).

### info_type.id (PK, int32_t)
- File: `info_type/id.bin` (rows = 113), dense identity.
- This query's use: target id for `it.info = 'top 250 rank'`.

### info_type.info (dimension text, varlen)
- Files: `info_type/info.off`, `info_type/info.dat` (rows = 113)
- This query's use: scan to resolve `'top 250 rank'` → `target_it_id`.

### movie_companies.movie_id (FK to title, int32_t)
- File: `movie_companies/movie_id.bin` (rows = 2609129); table is FK-sorted by movie_id.
- This query's use: join key with `title.id`; use the offsets_only index to iterate mc rows for a given movie id.

### movie_companies.company_type_id (FK to company_type, int32_t)
- File: `movie_companies/company_type_id.bin` (rows = 2609129)
- This query's use: `ct.id = mc.company_type_id` → test `company_type_id_bin[r] == target_ct_id`.

### movie_companies.note (varlen)
- Files: `movie_companies/note.off`, `movie_companies/note.dat` (rows = 2609129)
- This query's use: NOT LIKE `'%(as Metro-Goldwyn-Mayer Pictures)%'` AND (LIKE `'%(co-production)%'` OR LIKE `'%(presents)%'`). Scan via `memmem` on `.dat` using `[off[r], off[r+1])` slice. Length-prefilter: skip rows where `off[r+1]-off[r] < 14` (length of `'(presents)'` = 10; the NOT LIKE pattern alone is harmless for short rows so prefilter only the positive LIKE).
- Empty slice represents NULL → fails the positive OR clause.
- Also projected as `MIN(mc.note)`: read `[off[r], off[r+1])` only for surviving rows.

### movie_info_idx.movie_id (FK to title, int32_t)
- File: `movie_info_idx/movie_id.bin` (rows = 1380035); FK-sorted by movie_id.
- This query's use: `mi_idx.movie_id = t.id`; iterate via offsets_only index.

### movie_info_idx.info_type_id (FK to info_type, int32_t)
- File: `movie_info_idx/info_type_id.bin` (rows = 1380035)
- This query's use: filter `info_type_id_bin[r] == target_it_id`.

### title.id (PK, int32_t)
- File: `title/id.bin` (rows = 2528312); dense identity, row i ↔ id (i+1).
- This query's use: driver via title rows surviving filters; join key.

### title.title (varlen)
- Files: `title/title.off`, `title/title.dat` (rows = 2528312)
- This query's use: projected via `MIN(t.title)`; read only for surviving rows.

### title.production_year (int32_t, nullable)
- File: `title/production_year.bin` (rows = 2528312); NULLs are `INT32_MIN`.
- This query's use: projected via `MIN(t.production_year)`; no predicate in Q1a.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| company_type | 4 | dimension | id | — |
| info_type | 113 | dimension | id | — |
| title | 2,528,312 | dimension/fact | id | 100000 |
| movie_companies | 2,609,129 | fact | movie_id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |

## Query Analysis
- Join graph: `ct(id) ── mc(company_type_id)`, `it(id) ── mi_idx(info_type_id)`, `t(id) ── mc(movie_id)` and `t(id) ── mi_idx(movie_id)`. `t` is the central dense-PK dimension; `mc` and `mi_idx` are facts sorted on movie_id.
- Driver: iterate `movie_id v = 1..2528312`. For each, use `movie_companies__movie_id` and `movie_info_idx__movie_id` offsets_only indexes to fetch matching mc/mi_idx ranges. Probe predicates before doing the title varlen read.
- Filter selectivities: `ct.kind = 'production companies'` selects 1 of 4 ct ids; `it.info = 'top 250 rank'` selects 1 of 113 it ids. The `mi_idx[info_type_id]==target_it_id` filter is very selective (top-250 is rare). The mc NOT LIKE / LIKE OR clause is also highly selective on note text.
- Aggregation: `MIN(...)` only → result is a single row; maintain three running mins.
- LIKE / NOT LIKE: use `memmem` against `.dat` slice; on NULL (empty slice) the positive LIKEs are false, the NOT LIKE is vacuously true (handle by testing length > 0 before the OR).
- Output projection: read `mc.note` (varlen), `t.title` (varlen), `t.production_year` (int) only for surviving (mc,mi_idx,t) combinations; maintain min lexicographically for varlen and arithmetic min for year (skip INT32_MIN).

## Indexes
- `movie_companies__movie_id` (offsets_only): files `_idx/movie_companies__movie_id__offsets.bin` (int32, size 2528314). Use:
```cpp
int32_t lo = off[v]; int32_t hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* mc row r */ }
```
- `movie_info_idx__movie_id` (offsets_only): files `_idx/movie_info_idx__movie_id__offsets.bin` (int32, size 2528314). Same access pattern.

## Rules followed
- ct/it id lookups done by scanning their dense-PK varlen text columns (no invented codes).
- mc.note, t.title are varlen → `.off`+`.dat` only.
- No keyword/movie_keyword indexes used (not in this query).
- No claim of any index on mc.note text.
