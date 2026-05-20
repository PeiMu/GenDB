# Q26c Guide

## SQL
```sql
SELECT MIN(chn.name) AS character_name,
       MIN(mi_idx.info) AS rating,
       MIN(t.title) AS complete_hero_movie
FROM complete_cast AS cc, comp_cast_type AS cct1, comp_cast_type AS cct2,
     char_name AS chn, cast_info AS ci, info_type AS it2, keyword AS k,
     kind_type AS kt, movie_info_idx AS mi_idx, movie_keyword AS mk,
     name AS n, title AS t
WHERE cct1.kind = 'cast'
  AND cct2.kind LIKE '%complete%'
  AND chn.name IS NOT NULL
  AND (chn.name LIKE '%man%' OR chn.name LIKE '%Man%')
  AND it2.info = 'rating'
  AND k.keyword IN ('superhero','marvel-comics','based-on-comic','tv-special',
                    'fight','violence','magnet','web','claw','laser')
  AND kt.kind = 'movie'
  AND t.production_year > 2000
  AND kt.id = t.kind_id AND t.id = mk.movie_id AND t.id = ci.movie_id
  AND t.id = cc.movie_id AND t.id = mi_idx.movie_id
  AND chn.id = ci.person_role_id AND n.id = ci.person_id
  AND k.id = mk.keyword_id AND cct1.id = cc.subject_id
  AND cct2.id = cc.status_id AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4.
- Use: resolve `cast_id` (==`'cast'`) and `complete_status_set` (LIKE '%complete%').

### char_name.name (filter, varlen)
- Files: `char_name/name.off`, `char_name/name.dat`; rows: 3140339; dense PK.
- Use: prebuild `chn_set` of ids whose name contains `man` or `Man`.

### cast_info.movie_id / .person_role_id / .person_id
- Files: `cast_info/movie_id.bin`, `cast_info/person_role_id.bin` (nullable), `cast_info/person_id.bin`.
- Rows: 36244344; sorted by movie_id.

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`; rows: 113.
- Use: resolve `it2_id` for `'rating'`.

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`; rows: 134170.
- Use: 10-element `kw_set`.

### kind_type.kind (filter, varlen)
- Files: `kind_type/kind.off`, `kind_type/kind.dat`; rows: 7.
- Use: resolve `movie_kind_id`.

### movie_info_idx.movie_id / .info_type_id / .info
- Files: `movie_info_idx/movie_id.bin`, `movie_info_idx/info_type_id.bin`, `movie_info_idx/info.off`+`.dat`.
- Note: NO `info > 'X.Y'` predicate in Q26c — only the join + it2 filter.

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin`, `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.

### name.name (no-op join only)
- Files: `name/name.off`, `name/name.dat`. Not projected; only `n.id = ci.person_id` join (existence). Since name is dense PK and ci.person_id is a valid FK, this join always succeeds for non-NULL ci.person_id.

### title.id / .kind_id / .production_year / .title
- Files: `title/id.bin`, `title/kind_id.bin`, `title/production_year.bin`, `title/title.off`+`.dat`.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | n/a |
| info_type | 113 | dim | id | n/a |
| kind_type | 7 | dim | id | n/a |
| keyword | 134170 | dim | id | 100000 |
| char_name | 3140339 | dim (filter set) | id | 100000 |
| name | 4167491 | dim (join existence) | id | 100000 |
| title | 2528312 | driver | id | 100000 |
| complete_cast | 135086 | fact | movie_id | 100000 |
| movie_info_idx | 1380035 | fact | movie_id | 100000 |
| movie_keyword | 4523930 | fact | movie_id | 100000 |
| cast_info | 36244344 | fact | movie_id | 200000 |

## Query Analysis
- Identical join graph to Q26a/b. No `mi_idx.info` value predicate (just join), more keywords, looser year filter (>2000).
- Driver: title; filter year>2000 and kind_id==movie.
- Build: `cast_id`, `complete_status_set`, `it2_id`, `movie_kind_id`, `kw_set` (10), `chn_set`.
- Probe order: movie_keyword → complete_cast → movie_info_idx (just it2 filter) → cast_info (person_role_id ∈ chn_set).
- Output: 3 MINs (chn.name, mi_idx.info, t.title). Note: n is joined but not projected.
- LIKE notes: `%man%`/`%Man%` via memmem on char_name (~3%).

## Indexes
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin` (int32, 2528314).
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`.

Usage pattern:
```cpp
int32_t lo = off[t_id], hi = off[t_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* child row r */ }
```

Rules: index slot 0 = count of FK<1; varlen via `.off`+`.dat`; never invent dict codes.
