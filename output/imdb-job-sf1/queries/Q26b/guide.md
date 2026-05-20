# Q26b Guide

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
  AND k.keyword IN ('superhero','marvel-comics','based-on-comic','fight')
  AND kt.kind = 'movie'
  AND mi_idx.info > '8.0'
  AND t.production_year > 2005
  AND kt.id = t.kind_id AND t.id = mk.movie_id AND t.id = ci.movie_id
  AND t.id = cc.movie_id AND t.id = mi_idx.movie_id
  AND chn.id = ci.person_role_id AND n.id = ci.person_id
  AND k.id = mk.keyword_id AND cct1.id = cc.subject_id
  AND cct2.id = cc.status_id AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`; rows: 4; dense PK.
- Use: `cast_id` for `=='cast'`; gather LIKE-'%complete%' ids into a small int set.

### char_name.name (filter, varlen)
- Files: `char_name/name.off`, `char_name/name.dat`; rows: 3140339; dense PK.
- Use: pre-build int32 set of `chn_id` where name has substring `man` or `Man` (skip empty/NULL).

### cast_info.movie_id / .person_role_id / .person_id (join keys, int32_t)
- Files: `cast_info/movie_id.bin`, `cast_info/person_role_id.bin` (nullable), `cast_info/person_id.bin`. Rows: 36244344; sorted by movie_id.
- Use: probe via `cast_info__movie_id`; skip NULL_INT person_role_id.

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`; rows: 113; dense PK.
- Use: resolve `it2_id` for `'rating'`.

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`; rows: 134170; dense PK.
- Use: collect 4 ids into `kw_set`.

### kind_type.kind (filter, varlen)
- Files: `kind_type/kind.off`, `kind_type/kind.dat`; rows: 7.
- Use: resolve `movie_kind_id`.

### movie_info_idx.movie_id / .info_type_id / .info (join+filter+project)
- Files: `movie_info_idx/movie_id.bin`, `movie_info_idx/info_type_id.bin`, `movie_info_idx/info.off`+`.dat`.
- Rows: 1380035; sorted by movie_id.
- Use: range probe; require `info_type_id==it2_id` and `string_view(info) > "8.0"`.

### movie_keyword.movie_id / .keyword_id
- Files: `movie_keyword/movie_id.bin`, `movie_keyword/keyword_id.bin`; rows: 4523930; sorted.
- Use: range probe; test `keyword_id ∈ kw_set`.

### complete_cast.movie_id / .subject_id / .status_id
- Files: `complete_cast/movie_id.bin` (nullable), `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.
- Rows: 135086; sorted by movie_id.

### name.name (project, varlen)
- Files: `name/name.off`, `name/name.dat`; rows: 4167491.

### title.id / .kind_id / .production_year / .title
- Files: `title/id.bin`, `title/kind_id.bin`, `title/production_year.bin`, `title/title.off`+`.dat`.
- Rows: 2528312; dense PK.

## Table Stats
| Table | Rows | Role | Sort Order | Block Size |
|---|---|---|---|---|
| comp_cast_type | 4 | dim | id | n/a |
| info_type | 113 | dim | id | n/a |
| kind_type | 7 | dim | id | n/a |
| keyword | 134170 | dim | id | 100000 |
| char_name | 3140339 | dim (filter set) | id | 100000 |
| name | 4167491 | dim (project) | id | 100000 |
| title | 2528312 | driver | id | 100000 |
| complete_cast | 135086 | fact | movie_id | 100000 |
| movie_info_idx | 1380035 | fact | movie_id | 100000 |
| movie_keyword | 4523930 | fact | movie_id | 100000 |
| cast_info | 36244344 | fact | movie_id | 200000 |

## Query Analysis
- Same join graph as Q26a; tighter filters: `year>2005`, `mi_idx.info > '8.0'`, only 4 keywords.
- Driver: title; filter year and kind_id.
- Build once: `cast_id`, `complete_status_set`, `it2_id`, `movie_kind_id`, `kw_set` (4), `chn_set` (~3% of chn).
- Probe order per t_id: movie_keyword (kw_set, very selective ~0.02%) → complete_cast → movie_info_idx (filter `> '8.0'`, ~5% of rating rows) → cast_info (chn_set ∩ MIN(n.name)).
- Output: 3 MINs (chn.name, mi_idx.info, t.title). `MIN(n.name)` is NOT projected here.
- LIKE notes: `%man%` and `%Man%` substrings via memmem on char_name.

## Indexes
- `complete_cast__movie_id` (offsets_only): `_idx/complete_cast__movie_id__offsets.bin`, int32 length 2528314.
- `movie_info_idx__movie_id` (offsets_only): `_idx/movie_info_idx__movie_id__offsets.bin`, int32 length 2528314.
- `movie_keyword__movie_id` (offsets_only): `_idx/movie_keyword__movie_id__offsets.bin`, int32 length 2528314.
- `cast_info__movie_id` (offsets_only): `_idx/cast_info__movie_id__offsets.bin`, int32 length 2528314.

Usage (all four are identical patterns):
```cpp
int32_t lo = off[t_id], hi = off[t_id + 1];
for (int32_t r = lo; r < hi; ++r) { /* row r */ }
```

Rules: index slot 0 = count of FK<1; never invent dict codes; varlen via `.off`+`.dat`.
