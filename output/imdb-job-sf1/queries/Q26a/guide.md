# Q26a Guide

## SQL
```sql
SELECT MIN(chn.name) AS character_name,
       MIN(mi_idx.info) AS rating,
       MIN(n.name) AS playing_actor,
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
  AND mi_idx.info > '7.0'
  AND t.production_year > 2000
  AND kt.id = t.kind_id AND t.id = mk.movie_id AND t.id = ci.movie_id
  AND t.id = cc.movie_id AND t.id = mi_idx.movie_id
  AND chn.id = ci.person_role_id AND n.id = ci.person_id
  AND k.id = mk.keyword_id AND cct1.id = cc.subject_id
  AND cct2.id = cc.status_id AND it2.id = mi_idx.info_type_id;
```

## Column Reference

### comp_cast_type.kind (filter, varlen)
- Files: `comp_cast_type/kind.off`, `comp_cast_type/kind.dat`
- Rows: 4 (dense PK).
- Use: scan once to resolve `cast_id` (kind=='cast'); scan again to collect ALL ids where kind contains `"complete"` (LIKE '%complete%') into a small set/bitmask.

### char_name.name (filter, varlen)
- Files: `char_name/name.off`, `char_name/name.dat`; rows: 3140339; dense PK → id = row+1.
- Use: build int32 bitmap or hashset of surviving `chn_id`s. Scan rows; skip NULL (off[i]==off[i+1]); accept if `memmem(name,"man")` OR `memmem(name,"Man")`. ASCII case-sensitive matches both substrings.

### cast_info.movie_id (join key, int32_t)
- File: `cast_info/movie_id.bin`; rows: 36244344; sorted by movie_id.
- Use: range probe via `cast_info__movie_id` offsets_only.

### cast_info.person_role_id (join key, int32_t, nullable)
- File: `cast_info/person_role_id.bin`. Use: probe surviving `chn_id` set; skip NULL_INT.

### cast_info.person_id (join key, int32_t)
- File: `cast_info/person_id.bin`. Use: lookup into name (dense PK → row = person_id-1) to fetch n.name.

### info_type.info (filter, varlen)
- Files: `info_type/info.off`, `info_type/info.dat`; rows: 113; dense PK.
- Use: scan once to resolve `it2_id` where info=='rating'.

### keyword.keyword (filter, varlen)
- Files: `keyword/keyword.off`, `keyword/keyword.dat`; rows: 134170; dense PK.
- Use: scan once, push (i+1) into a `flat_hash_set<int32_t>` of matching keyword_ids (10 literals).

### kind_type.kind (filter, varlen)
- Files: `kind_type/kind.off`, `kind_type/kind.dat`; rows: 7.
- Use: resolve `movie_kind_id` where kind=='movie'.

### movie_info_idx.movie_id (join key, int32_t)
- File: `movie_info_idx/movie_id.bin`; rows: 1380035; sorted.
- Use: range probe via `movie_info_idx__movie_id`.

### movie_info_idx.info_type_id (join, int32_t)
- File: `movie_info_idx/info_type_id.bin`. Filter: `== it2_id`.

### movie_info_idx.info (filter+project, varlen)
- Files: `movie_info_idx/info.off`, `movie_info_idx/info.dat`.
- Use: lexicographic compare `string_view > "7.0"`. Also projected for MIN.

### movie_keyword.movie_id (join, int32_t)
- File: `movie_keyword/movie_id.bin`; rows: 4523930; sorted by movie_id.
- Use: range probe via `movie_keyword__movie_id`; test `keyword_id ∈ kw_set`.

### movie_keyword.keyword_id (filter, int32_t)
- File: `movie_keyword/keyword_id.bin`.

### complete_cast.movie_id (join, int32_t, nullable)
- File: `complete_cast/movie_id.bin`; rows: 135086; sorted.
- Use: range probe via `complete_cast__movie_id`.

### complete_cast.subject_id / status_id (filter, int32_t)
- Files: `complete_cast/subject_id.bin`, `complete_cast/status_id.bin`.
- Use: `subject_id == cast_id` AND `status_id ∈ complete_status_ids`.

### name.name (project, varlen)
- Files: `name/name.off`, `name/name.dat`; rows: 4167491; dense PK.
- Use: fetch via row = ci.person_id - 1 for MIN.

### char_name.name (project, same file as above) — used for MIN(chn.name).

### title.id / title.kind_id / title.production_year / title.title
- Files: `title/id.bin`, `title/kind_id.bin`, `title/production_year.bin`, `title/title.off`+`.dat`; rows: 2528312; dense PK.
- Use: driver. For row r: t_id=r+1, test `kind_id == movie_kind_id`, `production_year > 2000 && != NULL_INT`.

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
- Join graph: hub `t.id`. Facts join via movie_id (cc, ci, mk, mi_idx). `ci.person_role_id → chn.id`; `ci.person_id → n.id`; `mk.keyword_id → k.id`; `cc.subject_id → cct1`; `cc.status_id → cct2`; `mi_idx.info_type_id → it2`; `t.kind_id → kt`.
- Driver: `title` rows; filter `production_year > 2000` and `kind_id == movie_kind_id` first (both cheap int32).
- Build side (once):
  - `cast_id` from comp_cast_type/kind.
  - `complete_status_ids` set from comp_cast_type/kind LIKE '%complete%'.
  - `it2_id` (rating) from info_type/info.
  - `movie_kind_id` from kind_type/kind.
  - `kw_set` (10 ids) from keyword/keyword.
  - `chn_set` from char_name/name with `man`/`Man` substring.
- For each surviving t_id, probe in order of selectivity: movie_keyword (kw_set membership) → complete_cast (subject==cast & status in set) → movie_info_idx (it2 & info > '7.0') → cast_info (chn_set & fetch n.name).
- Selectivities (rough): year>2000 ~25%; keyword IN (10) ~0.05% of titles via mk; mi_idx.info>'7.0' ~20% of rating rows; chn ~3% of char_names.
- MIN aggregation: 4 MINs; track 4 best varlen string_views.
- LIKE notes: `%man%` and `%Man%` are case-sensitive memmem; `%complete%` on the 4-row dim is trivial.

## Indexes
- `complete_cast__movie_id` (offsets_only)
  - File: `_idx/complete_cast__movie_id__offsets.bin`; int32 length 2528314. Sentinel: empty if off[v]==off[v+1].
  - Use: `lo=off[t_id]; hi=off[t_id+1]; for r in [lo,hi): test cc.subject_id==cast_id && status_id∈set`.
- `movie_info_idx__movie_id` (offsets_only)
  - File: `_idx/movie_info_idx__movie_id__offsets.bin`; int32 length 2528314.
- `movie_keyword__movie_id` (offsets_only)
  - File: `_idx/movie_keyword__movie_id__offsets.bin`; int32 length 2528314.
- `cast_info__movie_id` (offsets_only)
  - File: `_idx/cast_info__movie_id__offsets.bin`; int32 length 2528314.

Rules: never invent dict codes; varlen reads use `.off`+`.dat`; index slot 0 holds NULL/<1 counts.
