# Q6f Guide

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
  AND t.production_year > 2000
  AND k.id = mk.keyword_id
  AND t.id = mk.movie_id
  AND t.id = ci.movie_id
  AND ci.movie_id = mk.movie_id
  AND n.id = ci.person_id;
```

## Column Reference

### keyword.keyword (filter+output, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`
- Row count: 134170 (dense PK).
- Use: scan; collect `target_k_ids` matching any of 8 literals. Then `MIN(k.keyword)` over surviving k_ids → pre-compute lexicographic min of the surviving keyword strings.

### keyword.id (join key, int32_t)
- File: `keyword/id.bin`; identity.

### movie_keyword.keyword_id (join key, int32_t)
- File: `movie_keyword/keyword_id.bin`
- Row count: 4523930.
- Use: CSR probe per id in target_k_ids.

### movie_keyword.movie_id (join key, int32_t)
- File: `movie_keyword/movie_id.bin`
- Use: derive `t_id`.

### name.name (output, varlen)
- Files: `name/name.off`, `name/name.dat`
- Row count: 4167491 (dense PK).
- Use: NO filter (Q6f drops the `%Downey%Robert%` predicate). Just MIN over names of every actor in a surviving movie.

### name.id (join key, int32_t)
- File: `name/id.bin`; identity. For each ci row, `n_id = ci_pid[r]`; row index `n_row = n_id - 1`.

### cast_info.person_id (join key, int32_t)
- File: `cast_info/person_id.bin`
- Row count: 36244344.
- Use: produces `n_id` for each ci row in t_id range.

### cast_info.movie_id (join key, int32_t)
- File: `cast_info/movie_id.bin`; sorted; offsets_only.

### title.id (join key, int32_t)
- File: `title/id.bin`; dense PK 2528312.

### title.production_year (filter, int32_t)
- File: `title/production_year.bin`
- Use: `production_year_bin[t_id - 1] > 2000 && != INT32_MIN`.

### title.title (output, varlen)
- Files: `title/title.off`, `title/title.dat`
- Use: MIN aggregate over surviving t_ids.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| keyword | 134170 | dim (filter) | id | 100000 |
| name | 4167491 | dim (project only) | id | 100000 |
| title | 2528312 | fact | id | 100000 |
| movie_keyword | 4523930 | driver | movie_id | 100000 |
| cast_info | 36244344 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: same as Q6a-e, but no name filter ⇒ every cast member of every surviving movie contributes to MIN(n.name) and MIN(t.title).
- Driver: target_k_ids → CSR `movie_keyword__keyword_id` → mk rows → distinct t_ids. Apply year>2000.
- For each surviving t_id: probe `cast_info__movie_id` range. Each ci row contributes its person_id; track current min n.name and current min t.title.
- Filter selectivities: 8 keywords; year>2000 ~33%; no name filter ⇒ much larger candidate cardinality than Q6b/d.
- MIN aggregations: 3 outputs. For MIN(n.name), iterate ci rows; for each `pid`, compare `name.name[pid-1]` against current min using offsets. Optimization: maintain seen-set of pids to skip duplicate reads.
- Output projection: read varlen offsets+data only for current candidate; minimal n.name read per unique pid.

## Indexes
- `movie_keyword__keyword_id` (CSR)
  - Files: `_idx/movie_keyword__keyword_id__offsets.bin` (int32 length `134172`), `_idx/movie_keyword__keyword_id__rowids.bin` (int32 length 4523930).
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
        int32_t pid = ci_person[r];
        /* compare name.name[pid-1] vs current min */
    }
    ```

## Rules
- Reference shared context for verbatim build code; only quote query-time usage.
- Never invent dictionary code values; resolve from .dict.off + .dict.dat at runtime.
- Never invent indexes not in build_indexes.cpp.
- varlen columns use `.off` + `.dat`, not `.bin`.
- No `%Downey%Robert%` predicate in this variant; name is project-only.
