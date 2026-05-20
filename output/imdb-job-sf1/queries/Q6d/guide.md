# Q6d Guide

## SQL
```sql
SELECT MIN(k.keyword) AS movie_keyword,
       MIN(n.name) AS actor_name,
       MIN(t.title) AS hero_movie
FROM cast_info AS ci,
     keyword AS k,
     movie_keyword AS mk,
     name AS n,
     title AS t
WHERE k.keyword IN ('superhero','sequel','second-part','marvel-comics',
                    'based-on-comic','tv-special','fight','violence')
  AND n.name LIKE '%Downey%Robert%'
  AND t.production_year > 2000
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
- Use: scan to collect 8 matching ids → `target_k_ids`.

### keyword.id (join key, int32_t)
- File: `keyword/id.bin`; identity.

### movie_keyword.keyword_id (join key, int32_t)
- File: `movie_keyword/keyword_id.bin`
- Row count: 4523930.
- Use: CSR probe for each id in target set.

### movie_keyword.movie_id (join key, int32_t)
- File: `movie_keyword/movie_id.bin`
- Use: derive `t_id`.

### name.name (filter, varlen)
- Files: `name/name.off`, `name/name.dat`
- Row count: 4167491 (dense PK).
- Use: pre-scan, build `downey_robert_ids`.

### name.id (join key, int32_t)
- File: `name/id.bin`; identity.

### cast_info.person_id (join key, int32_t)
- File: `cast_info/person_id.bin`
- Row count: 36244344.
- Use: per t_id range, test membership in `downey_robert_ids`.

### cast_info.movie_id (join key, int32_t)
- File: `cast_info/movie_id.bin`; sorted; offsets_only.

### title.id (join key, int32_t)
- File: `title/id.bin`; dense PK 2528312.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `production_year_bin[t_id - 1] > 2000 && != INT32_MIN`.

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
- Identical join shape to Q6b; only year predicate is `>2000` (looser ~33% of titles).
- Driver: enumerate target_k_ids → mk rows → unique t_ids; year>2000; ci probe; downey-robert match.
- MIN aggregations: 3 outputs.
- LIKE: `%Downey%Robert%`.
- IN: 8-literal varlen scan over keyword.keyword.
- Output projection: t.title and n.name varlen reads only for surviving (n,t) pairs; track minimum k.keyword over surviving k_ids.

## Indexes
- `movie_keyword__keyword_id` (CSR)
  - Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32, length `134172`), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, length 4523930).
  - Empty: `off[v]==off[v+1]`.
  - Access:
    ```cpp
    for (int32_t k_id : target_k_ids) {
        int32_t lo = mk_off[k_id], hi = mk_off[k_id + 1];
        for (int32_t j = lo; j < hi; ++j) {
            int32_t mk_row = mk_rowids[j];
            int32_t t_id  = mk_movie_id[mk_row];
        }
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
- Track min k.keyword among the k_ids that contributed to any surviving tuple.
