# Q25a Guide

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

## Column Reference

### cast_info.movie_id, .person_id, .note
- Files: `cast_info/{movie_id,person_id}.bin`, `note.off|.dat` (rows = 36244344); FK-sorted by movie_id.
- Use: offsets_only `cast_info__movie_id`; filter `note ∈ writer-set` (5 literals; `flat_hash_set<string_view>`; length prefilter). Then probe `valid_persons[person_id-1]` (male) bitset.

### info_type.id, .info
- Files: `info_type/id.bin`, `info.off|.dat` (rows = 113)
- Use: scan TWICE → `it1_id` ('genres') and `it2_id` ('votes'). Two separate ids on the same dimension.

### keyword.id, .keyword
- Files: `keyword/id.bin`, `keyword.off|.dat` (rows = 134170)
- Use: scan for each of `{hero, martial-arts, hand-to-hand-combat}`; build `kw_ids` (3 ids).

### movie_info.movie_id, .info_type_id, .info
- Files: `movie_info/{movie_id,info_type_id}.bin`, `info.off|.dat` (rows = 14835720); FK-sorted by movie_id.
- Use: offsets_only `movie_info__movie_id`; filter `info_type_id == it1_id`; then `info ∈ {Horror,Action,Sci-Fi,Thriller,Crime,War}` via `flat_hash_set<string_view>`. Also projected via `MIN(mi.info)`.

### movie_info_idx.movie_id, .info_type_id, .info
- Files: `movie_info_idx/{movie_id,info_type_id}.bin`, `info.off|.dat` (rows = 1380035); FK-sorted by movie_id.
- Use: offsets_only `movie_info_idx__movie_id`; filter `info_type_id == it2_id`. Projected via `MIN(mi_idx.info)`. SEPARATE table from movie_info — use its own CSR/offsets index.

### movie_keyword.movie_id, .keyword_id
- Files: `movie_keyword/{movie_id,keyword_id}.bin` (rows = 4523930); FK-sorted by movie_id.
- Use: offsets_only `movie_keyword__movie_id`; filter `kw_ids.contains(keyword_id)`.

### name.id, .name, .gender
- Files: `name/id.bin` (identity, rows = 4167491), `name.off|.dat`, `gender.bin|.dict.off|.dict.dat`
- Use: resolve `m_code` from gender dict. Pre-pass: build `valid_persons` bitset over `gender_bin[i] == m_code`. Project `MIN(n.name)` for surviving ci.person_id.

### title.id, .title
- Files: `title/id.bin`, `title.off|.dat` (rows = 2528312)
- Use: driver, no predicate on title or year (Q25a has no production_year filter). Project `MIN(t.title)`.

## Table Stats
| Table | Rows | Role | Sort order | Block size |
|---|---|---|---|---|
| info_type | 113 | dim (used twice) | id | — |
| keyword | 134,170 | dim | id | 100000 |
| name | 4,167,491 | dim | id | 100000 |
| title | 2,528,312 | driver | id | 100000 |
| movie_info_idx | 1,380,035 | fact | movie_id | 100000 |
| movie_keyword | 4,523,930 | fact | movie_id | 100000 |
| movie_info | 14,835,720 | fact | movie_id | 200000 |
| cast_info | 36,244,344 | fact | movie_id | 200000 |

## Query Analysis
- Join graph: title hub on movie_id (mi, mi_idx, ci, mk); ci→n on person_id; mi→it1, mi_idx→it2, mk→k.
- Resolve before driver: `it1_id` ('genres'), `it2_id` ('votes'), `kw_ids` (3), `m_code`. Pre-pass `valid_persons` bitset (gender == m_code).
- Driver: title v=1..2528312 (no predicate ⇒ all rows enter).
- Per-survivor v: order facts cheap-first.
  1. `mk` range → filter kw_ids (3 hot ids).
  2. `mi_idx` range → filter info_type_id == it2_id (votes).
  3. `mi` range → filter info_type_id == it1_id (genres) AND info ∈ 6-set.
  4. `ci` range → filter `note ∈ writer-set` AND `valid_persons[person_id-1]`.
- Each fact must yield ≥1 satisfying row. Movie_info and movie_info_idx are SEPARATE tables — use their dedicated offsets_only indexes; don't confuse them.
- Selectivities: `kw_ids` very selective (3/134170); `it1_id`/`it2_id` filter mi/mi_idx to ~1/113 of rows per movie; `note ∈ writers` ~few %; `gender == m_code` ~majority of names; `mi.info` 6-set selective once it1 is matched.
- MIN aggregation: running mins for mi.info, mi_idx.info, n.name, t.title.
- LIKE notes: none — all predicates are equality or IN-set.

## Indexes
- `movie_info__movie_id` (offsets_only): `_idx/movie_info__movie_id__offsets.bin` (int32, 2528314). Use for movie_info, NOT movie_info_idx.
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin` (int32, 2528314). Distinct file for movie_info_idx.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.

Snippet:
```cpp
int32_t lo = off[v], hi = off[v+1];
for (int32_t r = lo; r < hi; ++r) { /* fact row r */ }
```
- n fetched by direct dense-PK index `name.off[ci.person_id-1] .. name.off[ci.person_id]`.
- No invented indexes on note/info/keyword text.
