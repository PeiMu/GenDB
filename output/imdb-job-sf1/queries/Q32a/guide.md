# Q32a Guide

## SQL
```sql
SELECT MIN(lt.link) AS link_type, MIN(t1.title) AS first_movie,
       MIN(t2.title) AS second_movie
FROM keyword k, link_type lt, movie_keyword mk, movie_link ml,
     title t1, title t2
WHERE k.keyword='10,000-mile-club'
  AND mk.keyword_id = k.id
  AND t1.id = mk.movie_id
  AND ml.movie_id = t1.id
  AND ml.linked_movie_id = t2.id
  AND lt.id = ml.link_type_id
  AND mk.movie_id = t1.id;
```

## Column Reference

### keyword.keyword (varlen, rows=134170)
Files: `keyword/keyword.{off,dat}`. Scan to resolve `'10,000-mile-club'` → single `k_id` (i+1 where slice matches; length 16 prefilter).

### keyword.id (int32, rows=134170)
File: `keyword/id.bin` identity. row i ↔ id (i+1).

### movie_keyword.keyword_id, movie_id (int32, rows=4523930)
Files: `movie_keyword/{keyword_id,movie_id}.bin`. Use CSR `movie_keyword__keyword_id` to fetch the mk rows for `k_id` directly (very selective).

### movie_link.movie_id, linked_movie_id, link_type_id (int32, rows=29997)
Files: `movie_link/{movie_id,linked_movie_id,link_type_id}.bin`. Table is sorted by movie_id; use offsets_only `movie_link__movie_id` for t1 side.

### link_type.link (varlen, rows=18)
Files: `link_type/link.{off,dat}`. Projected via `MIN(lt.link)`. Row index = `link_type_id - 1` (dense PK).

### link_type.id (int32, rows=18)
File: `link_type/id.bin` identity.

### title.id (int32, rows=2528312)
File: `title/id.bin` identity. Both `t1` and `t2` use the same underlying files; aliases are purely conceptual.

### title.title (varlen, rows=2528312)
Files: `title/title.{off,dat}`. Both `t1.title` (at row `t1_id-1`) and `t2.title` (at row `t2_id-1`) read from the same file. Projected via two separate MINs.

## Table Stats
| Table | Rows | Role | Sort |
|---|---|---|---|
| keyword | 134,170 | dim | id |
| link_type | 18 | dim | id |
| title | 2,528,312 | dense-PK (t1+t2) | id |
| movie_keyword | 4,523,930 | fact | movie_id |
| movie_link | 29,997 | fact (self-join bridge) | movie_id |

## Query Analysis
- Join graph:
  ```
  k --(k.id=mk.keyword_id)-- mk --(mk.movie_id=t1.id)-- t1
                                          |
                              ml.movie_id = t1.id
                                          |
                                          ml --(ml.linked_movie_id=t2.id)-- t2
                                                   ml.link_type_id = lt.id
  ```
- Self-join: `t1` and `t2` share `title/*` files. Conceptual alias only; data layer has one set of arrays.
- Driver flow:
  1. Resolve `k_id` from `keyword/keyword.{off,dat}` (one id).
  2. Walk CSR `movie_keyword__keyword_id` slot for `k_id` → mk rowids → `t1_ids = {mk.movie_id[r]}` (small, often 1-10s).
  3. For each `t1_id`: use offsets_only `movie_link__movie_id` → ml row range. For each ml row:
     - read `t2_id = linked_movie_id[ml]` (direct dereference — title.id is dense, no extra index needed).
     - read `lt_id = link_type_id[ml]` → projected MIN at `link_type.link[lt_id-1]`.
     - MIN(t1.title) at title row `t1_id - 1`.
     - MIN(t2.title) at title row `t2_id - 1`.
- Selectivities: `'10,000-mile-club'` is rare; mk rows for it ≪ 100. ml rows per movie typically ≤ a handful. End-to-end result set is small.
- MIN aggregation: three running minima (lt.link, t1.title, t2.title). Read varlen slices only for surviving pairs.
- LIKE: none in Q32a.

## Indexes
- `movie_keyword__keyword_id` (CSR): `_idx/movie_keyword__keyword_id__offsets.bin` (int32, 134172), `_idx/movie_keyword__keyword_id__rowids.bin` (int32, 4523930).
  ```cpp
  auto mkk_off = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__offsets.bin");
  auto mkk_row = read_vec<int32_t>(store + "/_idx/movie_keyword__keyword_id__rowids.bin");
  int32_t lo = mkk_off[k_id], hi = mkk_off[k_id+1];
  for (int32_t k = lo; k < hi; ++k) {
      int32_t r = mkk_row[k];
      int32_t t1_id = mk_movie_id[r];
  }
  ```
- `movie_link__movie_id` (offsets_only): `_idx/movie_link__movie_id__offsets.bin` (int32, 2528314). Used for the t1 side.
  ```cpp
  int32_t lo = mlm_off[t1_id], hi = mlm_off[t1_id+1];
  for (int32_t r = lo; r < hi; ++r) {
      int32_t t2_id   = ml_linked_movie_id[r];
      int32_t lt_id   = ml_link_type_id[r];
  }
  ```
- t2 side: no CSR needed — title is dense PK, so `t2_id` directly addresses the title arrays at row `t2_id-1`.
- No invented indexes. Aliases t1/t2 share the same `title/*` files.
