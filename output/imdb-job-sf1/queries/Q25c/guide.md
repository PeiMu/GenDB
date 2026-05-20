# Q25c Guide

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
  AND mi.info IN ('Horror','Thriller')
  AND n.gender = 'm'
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
Differs from Q25a: `mi.info` IN-set narrowed to `('Horror','Thriller')`. No production_year or title filter.

## Column Reference

### cast_info.movie_id, .person_id, .note
- Files: `cast_info/{movie_id,person_id}.bin`, `note.off|.dat` (rows = 36244344); FK-sorted by movie_id.
- Use: offsets_only `cast_info__movie_id`; filter `note ∈ writer-set` (5 literals; `flat_hash_set<string_view>`; length prefilter), then `valid_persons[person_id-1]`.

### info_type.id, .info
- Files: `info_type/id.bin`, `info.off|.dat` (rows = 113)
- Use: scan TWICE → `it1_id` ('genres'), `it2_id` ('votes').

### keyword.id, .keyword
- Files: `keyword/id.bin`, `keyword.off|.dat` (rows = 134170)
- Use: scan; build `kw_ids` for the 3 literals.

### movie_info.movie_id, .info_type_id, .info
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat` (rows = 14835720)
- Use: filter `info_type_id == it1_id`. Then info ∈ {'Horror','Thriller'} — 2-element `flat_hash_set<string_view>` (or two memcmp's). Projected via `MIN(mi.info)`.

### movie_info_idx.movie_id, .info_type_id, .info
- Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.off|.dat` (rows = 1380035)
- Use: filter `info_type_id == it2_id`. Projected via `MIN(mi_idx.info)`. SEPARATE table from movie_info.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin` (rows = 4523930)
- Use: filter `kw_ids.contains(keyword_id)`.

### name.id, .name, .gender
- Files: `name/id.bin`, `name.off|.dat`, `gender.bin|.dict.off|.dict.dat` (int8 dict)
- Use: resolve `m_code` for `'m'`; pre-pass build `valid_persons` bitset where `gender_bin[i] == m_code`. Project `MIN(n.name)`.

### title.id, .title
- Files: `title/id.bin`, `title.off|.dat` (rows = 2528312)
- Use: driver; no predicate. Project `MIN(t.title)`.

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
- Same shape as Q25a with tighter mi.info IN-set (2 vs 6).
- Resolve before driver: `it1_id`, `it2_id`, `kw_ids` (3), `m_code`. Pre-pass `valid_persons` bitset.
- Driver: t row v=1..2528312 (no title or year predicate ⇒ all titles enter).
- Per-survivor v: order facts cheap-first.
  1. `mk` range → `kw_ids.contains(keyword_id)` (3 hot ids).
  2. `mi_idx` range → `info_type_id == it2_id`.
  3. `mi` range → `info_type_id == it1_id` AND `info ∈ {Horror, Thriller}`.
  4. `ci` range → `note ∈ writer-set` AND `valid_persons[person_id-1]`.
- Each fact must yield ≥1 satisfying row; early-fail per movie if any fact has no match.
- Selectivities: kw_ids 3/134170 → most movies fail at mk; mi.info ∈ {Horror,Thriller} narrower than Q25a.
- MIN aggregation: running mins on (mi.info, mi_idx.info, n.name, t.title).
- LIKE notes: none in Q25c.

## Indexes
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314). Used for movie_info only.
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314). Used for movie_info_idx — DIFFERENT row count and index from movie_info.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```
- n fetched by direct dense-PK index `name.off[ci.person_id-1] .. name.off[ci.person_id]`.
- No invented indexes on note/info/keyword text.
