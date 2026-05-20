# Q6a Guide

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
  AND t.production_year > 2010
  AND k.id = mk.keyword_id
  AND t.id = mk.movie_id
  AND t.id = ci.movie_id
  AND ci.movie_id = mk.movie_id
  AND n.id = ci.person_id;
```

## Column Reference

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`
- Row count: 134170 (dense PK → row i ↔ id (i+1)).
- Use: scan once to find `target_k_id` where bytes == `"marvel-cinematic-universe"`. Returns id; if not found ⇒ result empty.

### keyword.id (join key, int32_t)
- File: `keyword/id.bin`; identity.

### movie_keyword.keyword_id (join key, int32_t)
- File: `movie_keyword/keyword_id.bin`
- Row count: 4523930; sorted by movie_id, not keyword_id ⇒ use CSR index `movie_keyword__keyword_id` to fetch all mk rows with `keyword_id == target_k_id`.

### movie_keyword.movie_id (join key, int32_t)
- File: `movie_keyword/movie_id.bin`
- Use: for each mk_rowid from CSR ⇒ `t_id = mk_movie_id[mk_rowid]`.

### name.name (filter, varlen)
- Files: `name/name.off`, `name/name.dat`
- Row count: 4167491 (dense PK → row i ↔ id (i+1)).
- Use: pre-scan names; collect `name_ids` (set) where `memmem(name, "Downey") && memmem(name, "Robert")` AND `pos_of("Downey") < pos_of("Robert")` (LIKE `%Downey%Robert%`). Build `flat_hash_set<int32_t> downey_robert_ids`.

### name.id (join key, int32_t)
- File: `name/id.bin`; identity.

### cast_info.person_id (join key, int32_t)
- File: `cast_info/person_id.bin`
- Row count: 36244344; sorted by movie_id.
- Use: probe `cast_info__movie_id` offsets_only range for given t_id; test `ci_pid[r] ∈ downey_robert_ids`.

### cast_info.movie_id (join key, int32_t)
- File: `cast_info/movie_id.bin`; sorted; offsets_only index lookup.

### title.id (join key, int32_t)
- File: `title/id.bin`; dense PK 2528312.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `production_year_bin[t_id - 1] > 2010 && != INT32_MIN`.

### title.title (output, varlen)
- Files: `title/title.off`, `title/title.dat`
- Use: MIN aggregate per surviving t.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| keyword | 134170 | dim (filter) | id | 100000 |
| name | 4167491 | dim (filter) | id | 100000 |
| title | 2528312 | fact | id | 100000 |
| movie_keyword | 4523930 | driver | movie_id | 100000 |
| cast_info | 36244344 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: `k.id=mk.keyword_id`, `mk.movie_id=t.id=ci.movie_id`, `ci.person_id=n.id`.
- Driver: resolve `target_k_id` once; use CSR `movie_keyword__keyword_id` to enumerate matching mk rows (very small; typically <100 rows for one keyword).
- For each mk → t_id; check `production_year > 2010`. Look up `t.title` for MIN.
- For ci probe: scan cast_info range for that t_id via offsets_only; test if any ci.person_id is in `downey_robert_ids`.
- Pre-build `downey_robert_ids` by full scan of name.name (4.17M rows) — substring search.
- Filter selectivities: keyword equality → 1 id; name LIKE very selective (handful of rows); year>2010 ~12% titles; t∩mk further reduced.
- MIN aggregations: three (k.keyword, n.name, t.title). Note: with k.id fixed, `MIN(k.keyword)` is constant → just emit the literal once.
- LIKE: `%Downey%Robert%` requires Downey then Robert in order.
- Output projection: per surviving (t, n) combo, update three running mins (n.name read via offsets, t.title read via offsets).

## Indexes
- `movie_keyword__keyword_id` (CSR)
  - Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32, length `134172`), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, length 4523930).
  - Empty: `off[v]==off[v+1]`.
  - Access:
    ```cpp
    int32_t lo = mk_off[target_k_id], hi = mk_off[target_k_id + 1];
    for (int32_t j = lo; j < hi; ++j) {
        int32_t mk_row = mk_rowids[j];
        int32_t t_id  = mk_movie_id[mk_row];
        /* ... */
    }
    ```
- `cast_info__movie_id` (offsets_only)
  - File: `_idx/cast_info__movie_id__offsets.bin`, int32 length `2528314`.
  - Empty: `off[v]==off[v+1]`.
  - Access:
    ```cpp
    int32_t lo = ci_off[t_id], hi = ci_off[t_id + 1];
    for (int32_t r = lo; r < hi; ++r) {
        int32_t pid = ci_person[r];
        if (downey_robert_ids.contains(pid)) { /* matched */ }
    }
    ```

## Rules
- Reference shared context for verbatim build code; only quote query-time usage.
- Never invent dictionary code values; resolve from .dict.off + .dict.dat at runtime.
- Never invent indexes not in build_indexes.cpp.
- varlen columns use `.off` + `.dat`, not `.bin`.
- `MIN(k.keyword)` with a single fixed `k.id` is just the literal — no aggregation needed.
