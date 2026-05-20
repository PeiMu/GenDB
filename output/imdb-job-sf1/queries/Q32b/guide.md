# Q32b Guide

## SQL
```sql
SELECT MIN(lt.link) AS link_type, MIN(t1.title) AS first_movie,
       MIN(t2.title) AS second_movie
FROM keyword k, link_type lt, movie_keyword mk, movie_link ml,
     title t1, title t2
WHERE k.keyword='character-name-in-title'
  AND mk.keyword_id = k.id
  AND t1.id = mk.movie_id
  AND ml.movie_id = t1.id
  AND ml.linked_movie_id = t2.id
  AND lt.id = ml.link_type_id
  AND mk.movie_id = t1.id;
```
Identical shape to Q32a; only the keyword literal differs. `character-name-in-title` is a very common keyword in IMDB — expect a large `t1_ids` set.

## Column Reference

### keyword.keyword (varlen, rows=134170)
Files: `keyword/keyword.{off,dat}`. Scan to resolve `'character-name-in-title'` → single `k_id` (length 23 prefilter).

### keyword.id (int32, rows=134170)
File: `keyword/id.bin` identity.

### movie_keyword.keyword_id, movie_id (int32, rows=4523930)
Files: `movie_keyword/{keyword_id,movie_id}.bin`. CSR `movie_keyword__keyword_id` slot for `k_id` is wide (this keyword appears on many movies; expect tens of thousands of mk rows).

### movie_link.movie_id, linked_movie_id, link_type_id (int32, rows=29997)
Files: `movie_link/{movie_id,linked_movie_id,link_type_id}.bin`. Sorted by movie_id; offsets_only `movie_link__movie_id` for t1 side.

### link_type.link (varlen, rows=18)
Files: `link_type/link.{off,dat}`. Projected via MIN. Row index = `link_type_id - 1`.

### link_type.id (int32, rows=18)
File: `link_type/id.bin` identity.

### title.id (int32, rows=2528312)
File: `title/id.bin` identity. `t1` and `t2` aliases share the same files.

### title.title (varlen, rows=2528312)
Files: `title/title.{off,dat}`. Read at `t1_id-1` and `t2_id-1` (dense PK).

## Table Stats
| Table | Rows | Role | Sort |
|---|---|---|---|
| keyword | 134,170 | dim | id |
| link_type | 18 | dim | id |
| title | 2,528,312 | dense-PK (t1+t2) | id |
| movie_keyword | 4,523,930 | fact | movie_id |
| movie_link | 29,997 | fact (self-join) | movie_id |

## Query Analysis
- Same join graph as Q32a. Self-join via movie_link.
- Driver:
  1. Resolve `k_id` from `keyword/keyword.{off,dat}`.
  2. CSR `movie_keyword__keyword_id`[k_id] → many mk rowids → `t1_ids = {mk.movie_id[r]}`.
  3. Per `t1_id` walk `movie_link__movie_id`[t1_id..t1_id+1) → ml rows. Most t1_ids will have an empty range; skip cheaply.
  4. For each surviving ml row: `t2_id = linked_movie_id[r]`, `lt_id = link_type_id[r]`. Project MINs.
- Alternative: since the keyword is so common, may be faster to scan `movie_link` linearly (29,997 rows total) and probe each ml.movie_id against a hash set of `t1_ids`. Choose based on |t1_ids| vs |ml|.
- Selectivities: `character-name-in-title` is broad — likely on the order of 1e4–1e5 movies; but only ~30K ml rows total, so most t1 ranges are empty. Linear ml scan is cheap.
- MIN aggregation: three running minima.
- LIKE: none.

## Indexes
- `movie_keyword__keyword_id` (CSR): `_idx/movie_keyword__keyword_id__offsets.bin` (int32, 134172), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, 4523930).
- `movie_link__movie_id` (offsets_only): `_idx/movie_link__movie_id__offsets.bin` (int32, 2528314).
  ```cpp
  int32_t lo = mlm_off[t1_id], hi = mlm_off[t1_id+1];
  for (int32_t r = lo; r < hi; ++r) {
      int32_t t2_id = ml_linked_movie_id[r];
      int32_t lt_id = ml_link_type_id[r];
  }
  ```
- t2 side: direct dereference into title arrays at row `t2_id-1`. No CSR needed.
- Aliases t1/t2 share `title/*` files; alias is purely conceptual.
- No invented indexes.
