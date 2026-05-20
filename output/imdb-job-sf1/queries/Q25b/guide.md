# Q25b Guide

## SQL
```sql
SELECT MIN(mi.info) AS movie_budget,
       MIN(mi_idx.info) AS movie_votes,
       MIN(n.name) AS male_writer,
       MIN(t.title) AS violent_movie_title
FROM cast_info AS ci,
     info_type AS it1,
     info_type AS it2,
     keyword AS k,
     movie_info AS mi,
     movie_info_idx AS mi_idx,
     movie_keyword AS mk,
     name AS n,
     title AS t
WHERE ci.note IN ('(writer)','(head writer)','(written by)','(story)','(story editor)')
  AND it1.info = 'genres'
  AND it2.info = 'votes'
  AND k.keyword IN ('hero','martial-arts','hand-to-hand-combat')
  AND mi.info IN ('Horror','Action','Sci-Fi','Thriller','Crime','War')
  AND n.gender = 'm'
  AND t.production_year > 2010
  AND t.title LIKE 'M%'
  AND t.id = mi.movie_id
  AND t.id = mi_idx.movie_id
  AND t.id = ci.movie_id
  AND t.id = mk.movie_id
  AND ci.movie_id = mi.movie_id
  AND ci.movie_id = mi_idx.movie_id
  AND ci.movie_id = mk.movie_id
  AND mi.movie_id = mi_idx.movie_id
  AND mi.movie_id = mk.movie_id
  AND mi_idx.movie_id = mk.movie_id
  AND n.id = ci.person_id
  AND it1.id = mi.info_type_id
  AND it2.id = mi_idx.info_type_id
  AND k.id = mk.keyword_id;
```
Differs from Q25a: adds `t.production_year > 2010` AND `t.title LIKE 'M%'`.

## Column Reference

### cast_info.movie_id, .person_id, .note
- Files: `cast_info/{movie_id,person_id}.bin`, `note.off|.dat` (rows = 36244344); FK-sorted by movie_id.
- Use: offsets_only `cast_info__movie_id`; filter `note ∈ writer-set` and `valid_persons[person_id-1]`.

### info_type.id, .info
- Files: `info_type/id.bin`, `info.off|.dat` (rows = 113)
- Use: scan TWICE → `it1_id` ('genres'), `it2_id` ('votes').

### keyword.id, .keyword
- Files: `keyword/id.bin`, `keyword.off|.dat`
- Use: scan; build `kw_ids` for the 3 literals.

### movie_info.movie_id, .info_type_id, .info
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat`
- Use: filter `info_type_id == it1_id`, info ∈ 6-set. Projected via `MIN(mi.info)`.

### movie_info_idx.movie_id, .info_type_id, .info
- Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.off|.dat`
- Use: filter `info_type_id == it2_id`. Projected via `MIN(mi_idx.info)`. SEPARATE table — use its own offsets_only index.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin`
- Use: filter `kw_ids.contains(keyword_id)`.

### name.id, .name, .gender
- Files: `name/id.bin`, `name.off|.dat`, `gender.bin|.dict.off|.dict.dat`
- Use: resolve `m_code`; pre-pass `valid_persons` bitset.

### title.id, .title, .production_year
- Files: `title/id.bin`, `title.off|.dat`, `production_year.bin` (rows = 2528312)
- Use: driver; filter `production_year > 2010` AND `title LIKE 'M%'`. The 'M%' filter is a single-byte prefix check: read first byte of `title.dat` at offset `title.off[v-1]` and compare to 'M'. Skip empty (NULL) titles. Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| info_type | 113 | dim (used twice) | id | — |
| keyword | 134,170 | dim | id | 100000 |
| title | 2,528,312 | driver | id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| name | 4,167,491 | dim | id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |
| cast_info | 36,244,344 | fact | movie_id | 200000 |

## Query Analysis
- Same shape as Q25a plus two driver-side filters that cut the title pass to ~recent movies whose title starts with 'M'.
- Resolve before driver: `it1_id`, `it2_id`, `kw_ids` (3), `m_code`. Pre-pass `valid_persons` bitset.
- Driver: t row v with `production_year > 2010` AND single-byte prefix 'M'.
- Per-survivor: order facts cheap-first: mk (kw_ids), mi_idx (it2_id), mi (it1_id + 6-set), ci (writer note + valid_persons).
- Selectivities: title prefix 'M' ~6-8% of titles; year > 2010 narrows further; kw_ids 3/134170; writer-note ~few %; gender 'm' majority of names.
- MIN aggregation: running mins on (mi.info, mi_idx.info, n.name, t.title).
- LIKE: `t.title LIKE 'M%'` is a single-byte memcmp; `n.name` no LIKE in Q25b.

## Indexes
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314).
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314). SEPARATE from movie_info above.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```
- n fetched by dense-PK direct index.
- No invented indexes on title/note/info text.
