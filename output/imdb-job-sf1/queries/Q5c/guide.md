# Q5c Guide

## SQL
```sql
SELECT MIN(t.title) AS american_movie
FROM company_type AS ct,
     info_type AS it,
     movie_companies AS mc,
     movie_info AS mi,
     title AS t
WHERE ct.kind = 'production companies'
  AND mc.note NOT LIKE '%(TV)%'
  AND mc.note LIKE '%(USA)%'
  AND mi.info IN ('Sweden','Norway','Germany','Denmark',
                  'Swedish','Denish','Norwegian','German',
                  'USA','American')
  AND t.production_year > 1990
  AND t.id = mi.movie_id
  AND t.id = mc.movie_id
  AND mc.movie_id = mi.movie_id
  AND ct.id = mc.company_type_id
  AND it.id = mi.info_type_id;
```

## Column Reference

### company_type.kind (filter, varlen)
- Files: `company_type/kind.off`, `company_type/kind.dat`
- Row count: 4 (dense PK).
- Use: resolve `target_ct_id` for `"production companies"`.

### company_type.id (join key, int32_t)
- File: `company_type/id.bin`; identity.

### movie_companies.movie_id (join key, int32_t)
- File: `movie_companies/movie_id.bin`
- Sorted by movie_id; row count 2609129; offsets_only index probe.

### movie_companies.company_type_id (filter+join, int32_t)
- File: `movie_companies/company_type_id.bin`
- Use: `mc_ct[r] == target_ct_id`.

### movie_companies.note (filter, varlen)
- Files: `movie_companies/note.off`, `movie_companies/note.dat`
- Use: row r passes iff (note contains `"(USA)"`) AND NOT (note contains `"(TV)"`). NULL note (empty) → both LIKE and NOT LIKE evaluate to UNKNOWN ⇒ row rejected. So require `len>0 && memmem(note,"(USA)") && !memmem(note,"(TV)")`.
  Length prefilter: skip if `len < 5`.

### movie_info.movie_id (join key, int32_t)
- File: `movie_info/movie_id.bin`; sorted by movie_id; offsets_only.

### movie_info.info_type_id (join key, int32_t)
- File: `movie_info/info_type_id.bin`; not filtered.

### movie_info.info (filter, varlen)
- Files: `movie_info/info.off`, `movie_info/info.dat`
- Use: 10-literal IN-set; length-prefilter then check membership.

### info_type.id (join key, int32_t)
- File: `info_type/id.bin`; identity; no filter.

### title.id (driver, int32_t)
- File: `title/id.bin`; dense PK 2528312; row r → t_id=r+1.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `production_year_bin[r] > 1990 && production_year_bin[r] != INT32_MIN`.

### title.title (output, varlen)
- Files: `title/title.off`, `title/title.dat`
- Use: MIN aggregate; read only for surviving rows.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| company_type | 4 | dim | id | n/a |
| info_type | 113 | dim | id | n/a |
| title | 2528312 | driver | id | 100000 |
| movie_companies | 2609129 | fact | movie_id | 100000 |
| movie_info | 14835720 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: title hub; same as Q5a/b.
- Driver: titles with year>1990 (~60%).
- For each: probe mc range, require ct match, `(USA)` in note, no `(TV)` (NULL ⇒ reject).
- Then probe mi range, require info ∈ 10-literal set.
- Filter selectivities: ct match ~25% of mc; `(USA)` LIKE ~5% of mc.note; the AND-NOT prunes more; mi.info IN ~3-5% of mi.
- MIN aggregation: single result.
- LIKE / NOT LIKE: one positive substring, one negative substring; both required; NULL note rejected.
- IN: 10-element varlen set.
- Output projection: t.title only for surviving rows.

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
  - Same layout/access pattern.

## Rules
- Reference shared context for verbatim build code; only quote query-time usage.
- NULL handling: NULL `mc.note` (empty varlen) fails both `LIKE` and `NOT LIKE` per SQL three-valued logic.
- Never invent dictionary code values; resolve from .dict.off + .dict.dat at runtime.
- Never invent indexes not in build_indexes.cpp.
- varlen columns use `.off` + `.dat`, not `.bin`.
