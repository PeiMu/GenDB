# Q5a Guide

## SQL
```sql
SELECT MIN(t.title) AS typical_european_movie
FROM company_type AS ct,
     info_type AS it,
     movie_companies AS mc,
     movie_info AS mi,
     title AS t
WHERE ct.kind = 'production companies'
  AND mc.note LIKE '%(theatrical)%'
  AND mc.note LIKE '%(France)%'
  AND mi.info IN ('Sweden','Norway','Germany','Denmark',
                  'Swedish','Denish','Norwegian','German')
  AND t.production_year > 2005
  AND t.id = mi.movie_id
  AND t.id = mc.movie_id
  AND mc.movie_id = mi.movie_id
  AND ct.id = mc.company_type_id
  AND it.id = mi.info_type_id;
```

## Column Reference

### company_type.kind (filter, varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat`
- Row count: 4 (dense PK, `id_is_dense_1_to_N` → row i ↔ id (i+1))
- Use: scan to find `ct_id` where bytes == `"production companies"`; that id is the only `company_type_id` accepted in `mc`.

### company_type.id (join key, int32_t)
- File: `company_type/id.bin`
- Identity column; `id == row+1`.
- Use: produces `target_ct_id` for filtering `mc.company_type_id`.

### movie_companies.movie_id (join key, int32_t)
- File: `movie_companies/movie_id.bin`
- Row count: 2609129; sorted by movie_id.
- Use: range probe via `movie_companies__movie_id` offsets_only index keyed by `t.id`.

### movie_companies.company_type_id (filter+join, int32_t)
- File: `movie_companies/company_type_id.bin`
- Use: `mc_ct[r] == target_ct_id`.

### movie_companies.note (filter, varlen)
- Files: `movie_companies/note.off`, `movie_companies/note.dat`
- Use: scan via `memmem` on `.dat` using row offsets; needles `"(theatrical)"` AND `"(France)"`. Length prefilter: skip rows where `off[r+1]-off[r] < 22` (sum of needles); empty entry = NULL → skip.

### movie_info.movie_id (join key, int32_t)
- File: `movie_info/movie_id.bin`
- Row count: 14835720; sorted by movie_id.
- Use: range probe via `movie_info__movie_id` offsets_only index keyed by `t.id`.

### movie_info.info_type_id (join key, int32_t)
- File: `movie_info/info_type_id.bin`
- Use: needed for `it.id = mi.info_type_id` join (no filter on `it`, but column still projected for join semantics). It must be non-NULL (it.id is FK).

### movie_info.info (filter, varlen)
- Files: `movie_info/info.off`, `movie_info/info.dat`
- Use: build `flat_hash_set<string_view>` of 8 literals; for each candidate row, length-prefilter (max literal length is 9), then check set membership.

### info_type.id (join key, int32_t)
- File: `info_type/id.bin`
- Identity column; no filter, only join — any non-NULL `mi.info_type_id` matches some it row.

### title.id (join key, int32_t)
- File: `title/id.bin`
- Row count: 2528312 (dense PK → row i ↔ id i+1).
- Use: driver loop variable; for row r → `t_id = r + 1`.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `production_year_bin[r] > 2005 && production_year_bin[r] != INT32_MIN`.

### title.title (output projection, varlen)
- Files: `title/title.off`, `title/title.dat`
- Use: only fetch for surviving t-rows; aggregate MIN by lexicographic compare of `string_view(dat+off[r], off[r+1]-off[r])`.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| company_type | 4 | dim | id | n/a |
| info_type | 113 | dim (no filter) | id | n/a |
| title | 2528312 | driver | id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: `t.id` is hub; `mc.movie_id=t.id`, `mi.movie_id=t.id`; `ct.id=mc.company_type_id`; `it.id=mi.info_type_id`.
- Driver: scan `title` rows; first apply `production_year > 2005` (cheap int32 test).
- Build sides: pre-resolve `target_ct_id` once; build IN-set for `mi.info`.
- Filter selectivities (rough): `production_year > 2005` ~30% of titles; `mc.note LIKE '%(theatrical)%' AND '%(France)%'` very selective (~0.1%); `mi.info IN (...)` ~0.5% of mi rows; `ct.kind='production companies'` → 1 of 4.
- MIN aggregation: single row result; maintain current min `string_view`.
- LIKE patterns: two `LIKE '%...%'` on `mc.note`; both substrings required (AND).
- IN: 8-element set on `mi.info` (varlen).
- Output projection: read `t.title.off`+`t.title.dat` only when t-row survives both child checks.

Execution sketch:
1. Resolve `target_ct_id` from `company_type/kind.*`.
2. For each title row r with year>2005:
   - Use `movie_companies__movie_id` to get mc range; test mc.company_type_id and mc.note. If no mc passes, skip.
   - Use `movie_info__movie_id` to get mi range; test mi.info IN set. If no mi passes, skip.
   - Read t.title, update MIN.

## Indexes
- `movie_companies__movie_id` (offsets_only)
  - File: `_idx/movie_companies__movie_id__offsets.bin`
  - Layout: int32 array length `2528312 + 2`; empty slot sentinel: `off[v]==off[v+1]`.
  - Access:
    ```cpp
    int32_t lo = mc_off[t_id], hi = mc_off[t_id + 1];
    for (int32_t r = lo; r < hi; ++r) { /* mc row r */ }
    ```
- `movie_info__movie_id` (offsets_only)
  - File: `_idx/movie_info__movie_id__offsets.bin`
  - Layout: int32 array length `2528312 + 2`.
  - Access analogous.

## Rules
- Reference shared context for verbatim build code; only quote query-time usage.
- Never invent dictionary code values; load from .dict.off + .dict.dat at runtime (not applicable here; no dict cols touched).
- Never invent indexes not in build_indexes.cpp.
- varlen columns use `.off` + `.dat`, not `.bin`.
- aka_name not used here.
