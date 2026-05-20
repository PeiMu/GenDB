# Q5b Guide

## SQL
```sql
SELECT MIN(t.title) AS american_vhs_movie
FROM company_type AS ct,
     info_type AS it,
     movie_companies AS mc,
     movie_info AS mi,
     title AS t
WHERE ct.kind = 'production companies'
  AND mc.note LIKE '%(VHS)%'
  AND mc.note LIKE '%(USA)%'
  AND mc.note LIKE '%(1994)%'
  AND mi.info IN ('USA','America')
  AND t.production_year > 2010
  AND t.id = mi.movie_id
  AND t.id = mc.movie_id
  AND mc.movie_id = mi.movie_id
  AND ct.id = mc.company_type_id
  AND it.id = mi.info_type_id;
```

## Column Reference

### company_type.kind (filter, varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat`
- Row count: 4 (dense PK → id=row+1).
- Use: resolve `target_ct_id` once for `"production companies"`.

### company_type.id (join key, int32_t)
- File: `company_type/id.bin`
- Identity column.

### movie_companies.movie_id (join key, int32_t)
- File: `movie_companies/movie_id.bin`
- Row count: 2609129; sorted by movie_id.
- Use: range probe via offsets_only index keyed by `t.id`.

### movie_companies.company_type_id (filter+join, int32_t)
- File: `movie_companies/company_type_id.bin`
- Use: `mc_ct[r] == target_ct_id`.

### movie_companies.note (filter, varlen)
- Files: `movie_companies/note.off`, `movie_companies/note.dat`
- Use: scan via `memmem` on `.dat`; three needles `"(VHS)"`, `"(USA)"`, `"(1994)"` all required. Length prefilter: skip if length < 17 (sum). Empty = NULL → skip.

### movie_info.movie_id (join key, int32_t)
- File: `movie_info/movie_id.bin`
- Row count: 14835720; sorted by movie_id; offsets_only index.

### movie_info.info_type_id (join key, int32_t)
- File: `movie_info/info_type_id.bin`
- Use: join to `it.id`; not filtered.

### movie_info.info (filter, varlen)
- Files: `movie_info/info.off`, `movie_info/info.dat`
- Use: 2-literal IN-set `{"USA","America"}`; length prefilter then equality.

### info_type.id (join key, int32_t)
- File: `info_type/id.bin`
- No filter (any matching info_type_id ok).

### title.id (driver, int32_t)
- File: `title/id.bin`
- Row count: 2528312 (dense PK).
- Use: row r → `t_id = r+1`.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `production_year_bin[r] > 2010 && production_year_bin[r] != INT32_MIN`.

### title.title (output, varlen)
- Files: `title/title.off`, `title/title.dat`
- Use: fetch only when row survives; update MIN by lex compare.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| company_type | 4 | dim | id | n/a |
| info_type | 113 | dim | id | n/a |
| title | 2528312 | driver | id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: title hub; `mc.movie_id=t.id`, `mi.movie_id=t.id`; `ct.id=mc.company_type_id`; `it.id=mi.info_type_id`.
- Driver: scan title with year>2010 prefilter (~12% of titles).
- For surviving titles: probe `movie_companies__movie_id` range; require row with `company_type_id==target_ct_id` AND note containing all three substrings (very selective; tighter than Q5a).
- Then probe `movie_info__movie_id` range; require `info IN {USA, America}` (~1-2% of mi rows in matching ranges).
- MIN aggregation: single string result.
- LIKE: three `'%...%'` substrings, AND.
- IN: 2-literal varlen set.
- Output projection: read `t.title.off+.dat` only for surviving t.

## Indexes
- `movie_companies__movie_id` (offsets_only)
  - File: `_idx/movie_companies__movie_id__offsets.bin`
  - Layout: int32 length `2528314`; empty range = `off[v]==off[v+1]`.
  - Access:
    ```cpp
    int32_t lo = mc_off[t_id], hi = mc_off[t_id + 1];
    for (int32_t r = lo; r < hi; ++r) { /* mc row r */ }
    ```
- `movie_info__movie_id` (offsets_only)
  - File: `_idx/movie_info__movie_id__offsets.bin`
  - Same layout; same access pattern.

## Rules
- Reference shared context for verbatim build code; only quote query-time usage.
- Never invent dictionary code values; resolve from `.dict.off + .dict.dat` at runtime.
- Never invent indexes not in build_indexes.cpp.
- varlen columns use `.off` + `.dat`, not `.bin`.
- aka_name not used here.
