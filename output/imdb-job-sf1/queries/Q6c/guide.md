# Q6c Guide

## SQL
```sql
SELECT MIN(k.keyword) AS movie_keyword,
       MIN(n.name) AS actor_name,
       MIN(t.title) AS marvel_movie
FROM cast_info AS ci,
     keyword AS k,
     movie_keyword AS mk,
     name AS n,
     title AS t
WHERE k.keyword = 'marvel-cinematic-universe'
  AND n.name LIKE '%Downey%Robert%'
  AND t.production_year > 2014
  AND k.id = mk.keyword_id
  AND t.id = mk.movie_id
  AND t.id = ci.movie_id
  AND ci.movie_id = mk.movie_id
  AND n.id = ci.person_id;
```

## Column Reference

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`
- Row count: 134170 (dense PK).
- Use: pre-scan to resolve `target_k_id` once for `"marvel-cinematic-universe"`.

### keyword.id (join key, int32_t)
- File: `keyword/id.bin`; identity.

### movie_keyword.keyword_id (join key, int32_t)
- File: `movie_keyword/keyword_id.bin`
- Row count: 4523930.
- Use: CSR probe `movie_keyword__keyword_id` for `target_k_id`.

### movie_keyword.movie_id (join key, int32_t)
- File: `movie_keyword/movie_id.bin`
- Use: derive `t_id` per matched mk row.

### name.name (filter, varlen)
- Files: `name/name.off`, `name/name.dat`
- Row count: 4167491 (dense PK).
- Use: pre-scan to build `downey_robert_ids` matching `%Downey%Robert%`.

### name.id (join key, int32_t)
- File: `name/id.bin`; identity.

### cast_info.person_id (join key, int32_t)
- File: `cast_info/person_id.bin`
- Row count: 36244344.
- Use: per t_id range, test `ci_pid[r] ∈ downey_robert_ids`.

### cast_info.movie_id (join key, int32_t)
- File: `cast_info/movie_id.bin`; sorted; offsets_only.

### title.id (join key, int32_t)
- File: `title/id.bin`; dense PK 2528312.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `production_year_bin[t_id - 1] > 2014 && != INT32_MIN`.

### title.title (output, varlen)
- Files: `title/title.off`, `title/title.dat`
- Use: MIN aggregate.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| keyword | 134170 | dim (filter) | id | 100000 |
| name | 4167491 | dim (filter) | id | 100000 |
| title | 2528312 | fact | id | 100000 |
| movie_keyword | 4523930 | driver | movie_id | 100000 |
| cast_info | 36244344 | fact | movie_id | 200000 |

## Query Analysis
- Same shape as Q6a but tighter `production_year > 2014`.
- Driver: CSR probe on `movie_keyword__keyword_id` for the single Marvel keyword id → small mk slice → list of candidate t_ids.
- For each t_id: year>2014 (very selective ~5%), then offsets_only ci probe with Downey-Robert filter.
- Filter selectivities: keyword equality → 1; name LIKE very selective; year>2014 ~5%.
- MIN aggregations: 3 outputs; `MIN(k.keyword)` is constant literal.
- LIKE: `%Downey%Robert%`.
- IN: none (single keyword).
- Output projection: read t.title, n.name varlen only for surviving rows.

## Indexes
- `movie_keyword__keyword_id` (CSR)
  - Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32 length `134172`), `_idx/movie_keyword__keyword_id__rowids.bin` (int32 length 4523930).
  - Empty: `off[v]==off[v+1]`.
  - Access:
    ```cpp
    int32_t lo = mk_off[target_k_id], hi = mk_off[target_k_id + 1];
    for (int32_t j = lo; j < hi; ++j) {
        int32_t mk_row = mk_rowids[j];
        int32_t t_id  = mk_movie_id[mk_row];
    }
    ```
- `cast_info__movie_id` (offsets_only)
  - File: `_idx/cast_info__movie_id__offsets.bin`, int32 length `2528314`.
  - Access:
    ```cpp
    int32_t lo = ci_off[t_id], hi = ci_off[t_id + 1];
    for (int32_t r = lo; r < hi; ++r) {
        if (downey_robert_ids.contains(ci_person[r])) { /* matched */ }
    }
    ```

## Rules
- Reference shared context for verbatim build code; only quote query-time usage.
- Never invent dictionary code values; resolve from .dict.off + .dict.dat at runtime.
- Never invent indexes not in build_indexes.cpp.
- varlen columns use `.off` + `.dat`, not `.bin`.
- `MIN(k.keyword)` with a single fixed `k.id` is just the literal.
